/* I2S muxer is deprecated, as we will move to the RT1176 that does not need it. Kept here for
completeness */
#define USE_I2S_MUXER 0

#if USE_I2S_MUXER
#include <basalte/drivers/i2s_muxer.h>
#endif
#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/rtp_stream.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

#define SINK_MCAST_ADDR "239.0.1.1"
#define SINK_MCAST_PORT 5004

LOG_MODULE_REGISTER(main);

RTP_STREAM_DEFINE_STATIC(my_audio_stream);

static const struct device *i2s_dev = DEVICE_DT_GET(DT_CHOSEN(basalte_i2s_out));
#if USE_I2S_MUXER
static const struct device *i2s_muxer_dev = DEVICE_DT_GET(DT_NODELABEL(sai1_muxer));
#endif
static const struct device *dac_dev = DEVICE_DT_GET(DT_CHOSEN(basalte_dac));

#define SAMPLE_RATE      CONFIG_RTP_AUDIO_STREAM_SAMPLE_SAMPLE_RATE
#define I2S_BLOCK_SIZE   CONFIG_RTP_AUDIO_STREAM_SAMPLE_I2S_BLOCK_SIZE
#define RTP_BLOCK_SIZE   CONFIG_RTP_AUDIO_STREAM_SAMPLE_PAYLOAD_SIZE
#define PAYLOAD_DURATION CONFIG_RTP_AUDIO_STREAM_SAMPLE_PAYLOAD_DURATION_US

/* Log every 5 seconds */
#define LOG_INTERVAL_BLOCKS (5000000U / PAYLOAD_DURATION)

#define N_BLOCKS           CONFIG_RTP_AUDIO_STREAM_SAMPLE_N_BLOCKS
#define JITTER_BUFFER_SIZE CONFIG_RTP_AUDIO_STREAM_SAMPLE_JITTER_BUFFER_SIZE

/* Timeout right before I2S buffer runs empty */
#define STREAM_TIMEOUT_US ((JITTER_BUFFER_SIZE - 2) * PAYLOAD_DURATION)

#define THREAD_STACK_SIZE 4096
#define THREAD_PRIO       85

struct sample_stream_state {
	enum {
		STATE_NOT_READY,
		STATE_READY,
		STATE_WAITING,
		STATE_BUFFERING,
		STATE_RUNNING,
		STATE_ERROR,
	} state;

	union {
		uint32_t jitter_buf_counter;
	};
};

static void audio_stream_thread_function(void *p1, void *p2, void *p3);
static struct rtp_stream_codec_api codec_api;
static void dac_configure_worker(struct k_work *work);

static struct sample_stream_state stream_state;

/* Can't use normal K_MEMSLAB_DEFINE, as caching fucks with it */
static char __nocache
	__aligned(WB_UP(32)) _k_mem_slab_buf_audio_slab[(N_BLOCKS)*WB_UP(I2S_BLOCK_SIZE)];
static STRUCT_SECTION_ITERABLE(k_mem_slab, audio_slab) = Z_MEM_SLAB_INITIALIZER(
	audio_slab, _k_mem_slab_buf_audio_slab, WB_UP(I2S_BLOCK_SIZE), N_BLOCKS);

#if USE_I2S_MUXER
/* Slab for the muxer — must always cover all I2S_MUXER_NUM_CHANNELS hardware lanes,
 * not just the active DT clients, so words_per_ch stays correct when some clients
 * are disabled in the overlay. */
static char __nocache __aligned(WB_UP(
	32)) _i2s_slab_buf_muxer[(N_BLOCKS)*WB_UP(I2S_MUXER_NUM_CHANNELS * I2S_BLOCK_SIZE)];
static STRUCT_SECTION_ITERABLE(k_mem_slab, i2s_slab_muxer) =
	Z_MEM_SLAB_INITIALIZER(i2s_slab_muxer, _i2s_slab_buf_muxer,
			       WB_UP(I2S_MUXER_NUM_CHANNELS *I2S_BLOCK_SIZE), N_BLOCKS);
#endif /* USE_I2S_MUXER */

K_THREAD_DEFINE(audio_stream_thread, THREAD_STACK_SIZE, audio_stream_thread_function, NULL, NULL,
		NULL, THREAD_PRIO, 0, 0);
K_WORK_DEFINE(dac_configure_work, dac_configure_worker);

static struct rtp_stream_config stream_config = {
	.block_size = RTP_BLOCK_SIZE,
	.codec_api = &codec_api,
	.mem_slab = &audio_slab,
	.transport_type = RTP_TRANSPORT_NET_PKT,
};

static K_SEM_DEFINE(rtp_configured, 0, 1);
static K_SEM_DEFINE(i2s_started, 0, 1);

static bool dacs_configured;

static void dac_configure_worker(struct k_work *work)
{
	int ret;

	if (dacs_configured) {
		return;
	}

	ret = audio_codec_configure(dac_dev, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to configure dac (%d)", ret);
		return;
	}
	LOG_INF("Started DAC");

	dacs_configured = true;
}

static int codec_decode(void *data, size_t size, size_t *decoded_size, void *user_data)
{
	ARG_UNUSED(user_data);

	uint8_t *src = (uint8_t *)data + size - 3;
	uint8_t *dst = (uint8_t *)data + (4 * size) / 3 - 4;
	size_t n_samples = size / 3;
	uint8_t b0, b1, b2;

	for (size_t i = n_samples; i > 0; i--) {
		b0 = src[0];
		b1 = src[1];
		b2 = src[2];

		dst[3] = b0;
		dst[2] = b1;
		dst[1] = b2;
		dst[0] = 0x00;

		dst -= 4;
		src -= 3;
	}

	*decoded_size = n_samples * 4;

	return 0;
}

static struct rtp_stream_codec_api codec_api = {
	.decode = codec_decode,
};

static void audio_stream_thread_function(void *p1, void *p2, void *p3)
{
	struct i2s_config i2s_cfg;
	struct rtp_msg msg;
	int ret;

	/* Configure I2S stream */
	/* Actually 24, but dma can not handle this */
	i2s_cfg.word_size = 32U;
	i2s_cfg.channels = 2U;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.frame_clk_freq = SAMPLE_RATE;
	i2s_cfg.block_size = I2S_BLOCK_SIZE;
	i2s_cfg.timeout = STREAM_TIMEOUT_US / USEC_PER_MSEC;
	i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.mem_slab = &audio_slab;
	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
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
		case STATE_READY:
			ret = rtp_stream_trigger(&my_audio_stream, RTP_STREAM_START);
			if (ret < 0) {
				LOG_ERR("Failed to start rtp audio stream while ready (%d)", ret);
				stream_state.state = STATE_ERROR;
				break;
			}
			LOG_INF("Start RTP audio stream");

			stream_state.state = STATE_WAITING;
			break;
		case STATE_WAITING:
			LOG_INF("Waiting on first audio packet");

			ret = rtp_stream_read(&my_audio_stream, &msg, K_FOREVER);
			if (ret < 0) {
				LOG_ERR("Failed to read rtp audio stream while waiting (%d)", ret);
				stream_state.state = STATE_ERROR;
				break;
			}

			ret = i2s_write(i2s_dev, msg.data, msg.data_len);
			if (ret < 0) {
				LOG_ERR("Failed to write I2S while waiting (%d)", ret);
				k_mem_slab_free(&audio_slab, msg.data);
				stream_state.state = STATE_ERROR;
				break;
			}

			LOG_INF("Received first audio packet");

			stream_state.jitter_buf_counter = 1;
			stream_state.state = STATE_BUFFERING;
			break;
		case STATE_BUFFERING:
			if (stream_state.jitter_buf_counter++ == JITTER_BUFFER_SIZE) {
				ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
				if (ret < 0) {
					LOG_ERR("Failed to start I2S (%d)", ret);
					return;
				}

				LOG_INF("Started I2S transmitter");

				k_work_submit(&dac_configure_work);
				stream_state.state = STATE_RUNNING;
				break;
			}

			ret = rtp_stream_read(&my_audio_stream, &msg, K_MSEC(1000));
			if (ret < 0) {
				LOG_ERR("Failed to read rtp audio stream for buffer (%d)", ret);
				stream_state.state = STATE_ERROR;
				break;
			}

			ret = i2s_write(i2s_dev, msg.data, msg.data_len);
			if (ret < 0) {
				LOG_ERR("Failed to write I2S for buffer (%d)", ret);
				k_mem_slab_free(&audio_slab, msg.data);
				stream_state.state = STATE_ERROR;
				break;
			}

			break;
		case STATE_RUNNING:
			ret = rtp_stream_read(&my_audio_stream, &msg, K_USEC(STREAM_TIMEOUT_US));
			if (ret < 0) {
				LOG_ERR("Failed to read rtp audio stream during run (%d)", ret);
				stream_state.state = STATE_ERROR;
				break;
			}

			if (++counter % LOG_INTERVAL_BLOCKS == 0) {
				LOG_INF("Received %u blocks", LOG_INTERVAL_BLOCKS);
			}

			ret = i2s_write(i2s_dev, msg.data, msg.data_len);
			if (ret < 0) {
				LOG_ERR("Failed to write I2S for buffer during run (%d)", ret);
				k_mem_slab_free(&audio_slab, msg.data);
				stream_state.state = STATE_ERROR;
				break;
			}

			break;
		case STATE_ERROR:
			ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
			if (ret < 0) {
				LOG_ERR("Failed to drain I2S (%d)", ret);
			} else {
				LOG_WRN("Stopped I2S transmitter");
			}

			ret = rtp_stream_trigger(&my_audio_stream, RTP_STREAM_DROP);
			if (ret < 0) {
				LOG_ERR("Failed to stop rtp audio stream (%d)", ret);

				/* Retry again later */
				k_msleep(10);
				break;
			} else {
				LOG_WRN("Stopped RTP audio stream");
			}

			stream_state.state = STATE_READY;
			break;
		default:
			break;
		}
	}
}

int main(void)
{

	struct net_sockaddr_in sink_sockaddr_in;
	struct net_in_addr sink_in_addr;
	int ret;

	LOG_INF("Hello world from rtp audio stream sink sample!");

#if USE_I2S_MUXER
	/* Configure I2S muxer */
	struct i2s_config i2s_cfg;
	i2s_cfg.word_size = 32U;
	i2s_cfg.channels = 2U;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.frame_clk_freq = SAMPLE_RATE;
	i2s_cfg.block_size = BLOCK_SIZE;
	i2s_cfg.timeout = 50;
	i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.mem_slab = &i2s_slab_muxer;

	ret = i2s_muxer_configure(i2s_muxer_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure muxer (%d)", ret);
		return ret;
	}
#endif /* USE_I2S_MUXER */

	if (net_addr_pton(NET_AF_INET, SINK_MCAST_ADDR, &sink_in_addr) != 0) {
		LOG_ERR("Invalid address: %s", SINK_MCAST_ADDR);
		return -EINVAL;
	}

	sink_sockaddr_in.sin_family = NET_AF_INET;
	sink_sockaddr_in.sin_addr = sink_in_addr;
	sink_sockaddr_in.sin_port = net_htons(SINK_MCAST_PORT);

	stream_config.iface = net_if_get_default();
	stream_config.sock_addr = (struct net_sockaddr *)&sink_sockaddr_in;

	ret = rtp_stream_init(&my_audio_stream);
	if (ret < 0) {
		LOG_ERR("Failed to init audio stream (%d)", ret);
		return ret;
	}

	ret = rtp_stream_configure(&my_audio_stream, RTP_STREAM_ROLE_SINK, &stream_config);
	if (ret < 0) {
		LOG_ERR("Failed to configure rtp audio stream (%d)", ret);
		return ret;
	}

	stream_state.state = STATE_READY;

	LOG_INF("RTP audio stream ready");

	return 0;
}

BUILD_ASSERT(JITTER_BUFFER_SIZE > 2);
BUILD_ASSERT(CONFIG_RTP_AUDIO_STREAM_SAMPLE_JITTER_BUFFER_SIZE <= CONFIG_I2S_TX_BLOCK_COUNT);
BUILD_ASSERT(CONFIG_RTP_AUDIO_STREAM_SAMPLE_JITTER_BUFFER_SIZE <=
	     CONFIG_RTP_AUDIO_STREAM_SAMPLE_N_BLOCKS);
