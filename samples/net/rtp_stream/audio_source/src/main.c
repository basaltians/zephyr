#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/rtp_stream.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

#define SOURCE_MCAST_ADDR "239.0.1.1"
#define SOURCE_MCAST_PORT 5004

#define PAYLOAD_TYPE 97

LOG_MODULE_REGISTER(main);

RTP_STREAM_DEFINE_STATIC(my_audio_stream);

static const struct device *i2s_dev = DEVICE_DT_GET(DT_CHOSEN(basalte_i2s_in));
static const struct device *adc_dev = DEVICE_DT_GET(DT_CHOSEN(basalte_adc));

struct sample_stream_state {
	enum {
		STATE_NOT_READY,
		STATE_READY,
		STATE_RUNNING,
		STATE_ERROR,
		STATE_RETRY,
	} state;
};

static void audio_stream_thread_function(void *p1, void *p2, void *p3);
static struct rtp_stream_codec_api codec_api;

static struct sample_stream_state stream_state;

#define SAMPLE_RATE    CONFIG_RTP_AUDIO_STREAM_SAMPLE_SAMPLE_RATE
#define WORD_SIZE_B    3
#define N_CHANNELS     2
#define I2S_BLOCK_SIZE CONFIG_RTP_AUDIO_STREAM_SAMPLE_I2S_BLOCK_SIZE

/* Log every 5 seconds */
#define LOG_INTERVAL_BLOCKS (5000000U / CONFIG_RTP_AUDIO_STREAM_SAMPLE_PAYLOAD_DURATION_US)

#define N_BLOCKS CONFIG_RTP_AUDIO_STREAM_SAMPLE_N_BLOCKS

#define THREAD_STACK_SIZE 4096
#define THREAD_PRIO       85

/* Can't use normal K_MEMSLAB_DEFINE, as caching fucks with it */
static char __nocache
	__aligned(WB_UP(32)) _k_mem_slab_buf_i2s_slab[(N_BLOCKS)*WB_UP(I2S_BLOCK_SIZE)];
static STRUCT_SECTION_ITERABLE(k_mem_slab, audio_slab) = Z_MEM_SLAB_INITIALIZER(
	audio_slab, _k_mem_slab_buf_i2s_slab, WB_UP(I2S_BLOCK_SIZE), N_BLOCKS);

K_THREAD_DEFINE(audio_stream_thread, THREAD_STACK_SIZE, audio_stream_thread_function, NULL, NULL,
		NULL, THREAD_PRIO, 0, 0);

static struct rtp_stream_config stream_config = {
	.codec_api = &codec_api,
	.payload_type = PAYLOAD_TYPE,
	.transport_type = RTP_TRANSPORT_NET_PKT,
};

/* Convert le32 to be24 */
static int codec_encode(void *data, size_t size, size_t *encoded_size, uint32_t *delta_ts,
			void *user_data)
{
	uint8_t *src = (uint8_t *)data;
	uint8_t *dst = (uint8_t *)data;
	size_t n_samples = size / 4;

	/* Convert each little-endian 32-bit sample [b0(pad), b1, b2, b3(MSB)]
	 * to big-endian 24-bit [b3, b2, b1], discarding the zero-padding LSB.
	 */
	for (size_t i = 0; i < n_samples; i++) {
		uint8_t b1 = src[1], b2 = src[2], b3 = src[3];
		dst[0] = b3;
		dst[1] = b2;
		dst[2] = b1;
		dst += 3;
		src += 4;
	}

	*encoded_size = n_samples * 3;
	*delta_ts = *encoded_size / (WORD_SIZE_B * N_CHANNELS);

	return 0;
}

static struct rtp_stream_codec_api codec_api = {
	.encode = codec_encode,
};

static void audio_stream_thread_function(void *p1, void *p2, void *p3)
{
	struct i2s_config i2s_cfg;
	void *memblock = NULL;
	uint32_t block_size;
	int ret;

	/* Configure I2S stream */
	/* Actually 24, but dma can not handle this */
	i2s_cfg.word_size = 32U;
	i2s_cfg.channels = 2U;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.frame_clk_freq = SAMPLE_RATE;
	i2s_cfg.block_size = I2S_BLOCK_SIZE;
	i2s_cfg.timeout = 50;
	i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.mem_slab = &audio_slab;
	ret = i2s_configure(i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure I2S stream\n");
		return;
	}
	LOG_INF("I2S configured");

	int counter = 0;
	while (true) {
		switch (stream_state.state) {
		case STATE_NOT_READY:
			/* Do nothing */
			k_msleep(10);
			break;
		case STATE_RETRY:
			k_msleep(1000);
			LOG_INF("Retrying");
			__fallthrough;
		case STATE_READY:
			ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
			if (ret < 0) {
				LOG_ERR("Failed to start I2S (%d)", ret);
				stream_state.state = STATE_ERROR;
				break;
			}
			LOG_INF("Start I2S receiver");

			ret = rtp_stream_trigger(&my_audio_stream, RTP_STREAM_START);
			if (ret < 0) {
				LOG_ERR("Failed to start rtp audio stream (%d)", ret);
				stream_state.state = STATE_ERROR;
				break;
			}
			LOG_INF("Start RTP audio stream");

			stream_state.state = STATE_RUNNING;
			break;
		case STATE_RUNNING:
			ret = i2s_read(i2s_dev, &memblock, &block_size);
			if (ret == -EAGAIN) {
				continue;
			}
			if (ret < 0) {
				LOG_ERR("Failed to read I2S (%d)", ret);

				if (memblock != NULL) {
					k_mem_slab_free(&audio_slab, memblock);
				}

				stream_state.state = STATE_ERROR;
				break;
			}

			ret = rtp_stream_write(&my_audio_stream, memblock, block_size, NULL);
			if (ret < 0) {
				LOG_ERR("Failed to write to rtp audio stream (%d)", ret);
				k_mem_slab_free(&audio_slab, memblock);
				stream_state.state = STATE_ERROR;
				break;
			}

			if (++counter % LOG_INTERVAL_BLOCKS == 0) {
				LOG_INF("Streamed %u blocks", LOG_INTERVAL_BLOCKS);
			}

			k_mem_slab_free(&audio_slab, memblock);
			break;
		case STATE_ERROR:
			ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
			if (ret < 0) {
				LOG_ERR("Failed to drop I2S (%d)", ret);
			} else {
				LOG_WRN("Stopped I2S receiver");
			}

			ret = rtp_stream_trigger(&my_audio_stream, RTP_STREAM_STOP);
			if (ret < 0) {
				LOG_ERR("Failed to stop rtp audio stream (%d)", ret);
			} else {
				LOG_WRN("Stopped RTP audio stream");
			}

			stream_state.state = STATE_RETRY;
			break;
		default:
			break;
		}
	}
}

int main(void)
{
	struct net_sockaddr_in source_sockaddr_in;
	struct net_in_addr source_in_addr;
	int ret;

	LOG_INF("Hello world from rtp audio stream source sample!");

	if (net_addr_pton(NET_AF_INET, SOURCE_MCAST_ADDR, &source_in_addr) != 0) {
		LOG_ERR("Invalid address: %s", SOURCE_MCAST_ADDR);
		return -EINVAL;
	}

	source_sockaddr_in.sin_family = NET_AF_INET;
	source_sockaddr_in.sin_addr = source_in_addr;
	source_sockaddr_in.sin_port = net_htons(SOURCE_MCAST_PORT);

	stream_config.iface = net_if_get_default();
	stream_config.sock_addr = (struct net_sockaddr *)&source_sockaddr_in;

	/* Wait a bit so I2S clocks are present when configuring ADC */
	k_msleep(100);

	audio_codec_start_output(adc_dev);
	LOG_INF("Started ADC");

	ret = rtp_stream_init(&my_audio_stream);
	if (ret < 0) {
		LOG_ERR("Failed to init audio stream (%d)", ret);
		return ret;
	}

	ret = rtp_stream_configure(&my_audio_stream, RTP_STREAM_ROLE_SOURCE, &stream_config);
	if (ret < 0) {
		LOG_ERR("Failed to configure rtp audio stream (%d)", ret);
		return ret;
	}

	stream_state.state = STATE_READY;

	LOG_INF("RTP audio stream ready");

	return 0;
}
