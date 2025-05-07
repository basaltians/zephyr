/**
 * Copyright (c) 2025 Basalte bv
 * SPDX-License-Identifier: Apache-2.0
 */


#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <openthread/platform/radio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/hdlc_rcp_if/hdlc_rcp_if.h>
#include <zephyr/net/ieee802154_radio.h>
#include <zephyr/net/openthread.h>

#define DT_DRV_COMPAT spi_hdlc_rcp_if

LOG_MODULE_REGISTER(hdlc_rcp_if_spi, CONFIG_HDLC_RCP_IF_DRIVER_LOG_LEVEL);

#define SPI_HEADER_LEN 5
#define SPI_HEADER_RESET_FLAG 0x80
#define SPI_HEADER_CRC_FLAG 0x40
#define SPI_HEADER_PATTERN_VALUE 0x02
#define SPI_HEADER_PATTERN_MASK 0x03

#define BUFFER_SIZE                                                                                \
	(SPI_HEADER_LEN + CONFIG_HDLC_RCP_IF_SPI_MAX_FRAME_SIZE +                                  \
	 CONFIG_HDLC_RCP_IF_SPI_ALIGN_ALLOWANCE)

struct hdlc_rcp_if_spi_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec int_gpio;
	struct gpio_dt_spec rst_gpio;
	uint16_t reset_time;
	uint16_t reset_delay;
};

struct hdlc_rcp_if_spi_data {
	const struct device *self;
	struct openthread_context *ot_context;
	struct k_work work;

	struct gpio_callback int_gpio_cb;
	hdlc_rx_callback_t rx_cb;
	void *rx_param;

	uint16_t peer_data_len;
	uint8_t rx_buf[BUFFER_SIZE];
	uint16_t rx_len;
	uint8_t tx_buf[BUFFER_SIZE];
	uint16_t tx_len;

	bool tx_ready;
	bool frame_sent;
};

BUILD_ASSERT(CONFIG_HDLC_RCP_IF_SPI_SMALL_PACKET_SIZE <=
		     CONFIG_HDLC_RCP_IF_SPI_MAX_FRAME_SIZE - SPI_HEADER_LEN,
	     "HDLC IF SPI small packet size larger than maximum frame size");

static int hdlc_rcp_if_spi_reset(const struct device *dev)
{
	const struct hdlc_rcp_if_spi_config *cfg = dev->config;

	if (cfg->rst_gpio.port != NULL) {
		int ret;

		if (!gpio_is_ready_dt(&cfg->rst_gpio)) {
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&cfg->rst_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			return ret;
		}

		k_msleep(cfg->reset_time);

		ret = gpio_pin_set_dt(&cfg->rst_gpio, 0);
		if (ret < 0) {
			return ret;
		}

		k_msleep(cfg->reset_delay);
	}

	return 0;
}

static void hdlc_rcp_if_push_pull_spi(struct k_work *work)
{
	struct hdlc_rcp_if_spi_data *data =
		CONTAINER_OF(work, struct hdlc_rcp_if_spi_data, work);
	const struct hdlc_rcp_if_spi_config *cfg = data->self->config;
	struct spi_buf tx_frame = {
		.buf = data->tx_buf,
		.len = data->tx_len + SPI_HEADER_LEN,
	};
	const struct spi_buf_set tx_set = {
		.buffers = &tx_frame,
		.count = 1U,
	};
	struct spi_buf rx_frame = {
		.buf = data->rx_buf,
		.len = SPI_HEADER_LEN + CONFIG_HDLC_RCP_IF_SPI_ALIGN_ALLOWANCE,
	};
	const struct spi_buf_set rx_set = {
		.buffers = &rx_frame,
		.count = 1U,
	};
	uint8_t *rx_buf;
	uint16_t peer_max_rx;
	int ret;

	data->tx_buf[0] = SPI_HEADER_PATTERN_VALUE;
	if (!data->frame_sent) {
		data->tx_buf[0] |= SPI_HEADER_RESET_FLAG;
	}

	sys_put_le16(0U, &data->tx_buf[1]);
	sys_put_le16(data->tx_len, &data->tx_buf[3]);

	if (data->rx_len == 0U) {
		if (data->peer_data_len > 0U) {
			rx_frame.len += data->peer_data_len;
			sys_put_le16(data->peer_data_len, &data->tx_buf[1]);
		} else {
			rx_frame.len += CONFIG_HDLC_RCP_IF_SPI_SMALL_PACKET_SIZE;
			sys_put_le16(CONFIG_HDLC_RCP_IF_SPI_SMALL_PACKET_SIZE, &data->tx_buf[1]);
		}
	}

	ret = spi_transceive_dt(&cfg->bus, &tx_set, &rx_set);
	if (ret < 0) {
		LOG_ERR("Failed to push/pull frames (%d)", ret);
		return;
	}

	/* Find the real start of the frame */
	rx_buf = data->rx_buf;
	for (uint8_t i = 0U; i < CONFIG_HDLC_RCP_IF_SPI_ALIGN_ALLOWANCE; ++i) {
		if (rx_buf[0] != 0xff) {
			break;
		}
		rx_buf++;
	}

	if ((rx_buf[0] & SPI_HEADER_PATTERN_MASK) != SPI_HEADER_PATTERN_VALUE) {
		LOG_HEXDUMP_WRN(rx_buf, SPI_HEADER_LEN, "Invalid header data");
		return;
	}

	peer_max_rx = sys_get_le16(&rx_buf[1]);
	data->rx_len = sys_get_le16(&rx_buf[3]);

	if (peer_max_rx > CONFIG_HDLC_RCP_IF_SPI_MAX_FRAME_SIZE ||
	    data->rx_len > CONFIG_HDLC_RCP_IF_SPI_MAX_FRAME_SIZE) {
		LOG_HEXDUMP_WRN(rx_buf, SPI_HEADER_LEN, "Invalid accept/data lengths");
		data->rx_len = 0;
		return;
	}

	data->frame_sent = true;

	if ((rx_buf[0] & SPI_HEADER_RESET_FLAG) != 0U) {
		LOG_DBG("Peer did reset");
	}

	if (data->rx_len > 0U && data->rx_cb != NULL) {
		data->rx_cb(&data->rx_buf[SPI_HEADER_LEN], data->rx_len, data->rx_param);
	}

	if (data->tx_len > peer_max_rx) {
		LOG_WRN("Peer not ready to receive our frame (%u > %u)", data->tx_len, peer_max_rx);
	}

	data->tx_ready = true;
}

static void hdlc_rcp_if_spi_isr(const struct device *port, struct gpio_callback *cb,
				gpio_port_pins_t pins)
{
	struct hdlc_rcp_if_spi_data *data =
		CONTAINER_OF(cb, struct hdlc_rcp_if_spi_data, int_gpio_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_work_submit(&data->work);
}

static void hdlc_iface_init(struct net_if *iface)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct hdlc_rcp_if_spi_data *data = dev->data;
	otExtAddress eui64;

	__ASSERT_NO_MSG(dev == net_if_get_device(iface));

	ieee802154_init(iface);
	data->ot_context = net_if_l2_data(iface);

	otPlatRadioGetIeeeEui64(ctx->ot_context->instance, eui64.m8);
	net_if_set_link_addr(iface, eui64.m8, OT_EXT_ADDRESS_SIZE, NET_LINK_IEEE802154);
}

static int hdlc_register_rx_cb(hdlc_rx_callback_t hdlc_rx_callback, void *param)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct hdlc_rcp_if_spi_data *data = dev->data;

	data->rx_cb = hdlc_rx_callback;
	data->rx_param = param;

	return 0;
}

static int hdlc_send(const uint8_t *frame, uint16_t length)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct hdlc_rcp_if_spi_data *data = dev->data;

	if (frame == NULL || length > CONFIG_HDLC_RCP_IF_SPI_MAX_FRAME_SIZE) {
		return -EINVAL;
	}

	if (!data->tx_ready) {
		return -EBUSY;
	}

	memcpy(&data->tx_buf[SPI_HEADER_LEN], frame, length);
	data->tx_len = length;
	data->tx_ready = false;

	k_work_submit(&data->work);

	return 0;
}

static int hdlc_deinit(void)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct hdlc_rcp_if_spi_data *data = dev->data;

	data->frame_sent = false;

	return 0;
}

static int hdlc_rcp_if_spi_init(const struct device *dev)
{
	const struct hdlc_rcp_if_spi_config *cfg = dev->config;
	struct hdlc_rcp_if_spi_data *data = dev->data;
	int ret;

	data->self = dev;
	k_work_init(&data->work, hdlc_rcp_if_push_pull_spi);

	if (!spi_is_ready_dt(&cfg->bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->int_gpio)) {
		LOG_ERR("Interrupt GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure interrupt GPIO pin (%d)", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure interrupt GPIO (%d)", ret);
		return ret;
	}

	gpio_init_callback(&data->int_gpio_cb, hdlc_rcp_if_spi_isr, BIT(cfg->int_gpio.pin));

	ret = gpio_add_callback(cfg->int_gpio.port, &data->int_gpio_cb);
	if (ret < 0) {
		LOG_ERR("Failed to add interrupt GPIO callback (%d)", ret);
		return ret;
	}

	ret = hdlc_rcp_if_spi_reset(dev);
	if (ret < 0) {
		LOG_ERR("Failed to reset HDLC SPI device (%d)", ret);
	}

	return 0;
}

static const struct hdlc_api spi_hdlc_api = {
	.iface_api.init = hdlc_iface_init,
	.register_rx_cb = hdlc_register_rx_cb,
	.send = hdlc_send,
	.deinit = hdlc_deinit,
};

#define L2_CTX_TYPE NET_L2_GET_CTX_TYPE(OPENTHREAD_L2)

#define MTU 1280

static const struct hdlc_rcp_if_spi_config ot_hdlc_rcp_cfg = {
	.bus = SPI_DT_SPEC_INST_GET(0, SPI_OP_MODE_MASTER | SPI_WORD_SET(8),
				    DT_INST_PROP(0, cs_delay)),
	.int_gpio = GPIO_DT_SPEC_INST_GET(0, int_gpios),
	.rst_gpio = GPIO_DT_SPEC_INST_GET_OR(0, reset_gpios, {}),
	.reset_time = DT_INST_PROP(0, reset_assert_time),
	.reset_delay = DT_INST_PROP(0, reset_delay),
};

static struct hdlc_rcp_if_spi_data ot_hdlc_rcp_data;

NET_DEVICE_DT_INST_DEFINE(0, hdlc_rcp_if_spi_init, NULL, &ot_hdlc_rcp_data, &ot_hdlc_rcp_cfg,
			  CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &spi_hdlc_api, OPENTHREAD_L2,
			  NET_L2_GET_CTX_TYPE(OPENTHREAD_L2), MTU);
