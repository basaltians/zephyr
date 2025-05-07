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
#include <zephyr/sys/ring_buffer.h>

#define DT_DRV_COMPAT spi_hdlc_rcp_if

LOG_MODULE_REGISTER(hdlc_rcp_if_spi, CONFIG_HDLC_RCP_IF_DRIVER_LOG_LEVEL);

struct hdlc_rcp_if_spi_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec int_gpio;
	struct gpio_dt_spec rst_gpio;
};

struct hdlc_rcp_if_spi_data {
	const struct device *self;
	struct openthread_context *ot_context;

	struct gpio_callback int_gpio_cb;
	struct k_work rx_work;
	hdlc_rx_callback_t rx_cb;
	void *rx_param;

	uint8_t rx_buf[CONFIG_OPENTHREAD_HDLC_RCP_IF_SPI_RX_BUFFER_SIZE];
};

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

		k_msleep(30);

		ret = gpio_pin_set_dt(&cfg->rst_gpio, 0);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static void hdlc_rcp_if_spi_rx_handler(struct k_work *work)
{
	struct hdlc_rcp_if_spi_data *data =
		CONTAINER_OF(work, struct hdlc_rcp_if_spi_data, rx_work);
	const struct hdlc_rcp_if_spi_config *cfg = data->self->config;
	uint8_t hdr[5];
	struct spi_buf buf = {
		.buf = hdr,
		.len = sizeof(hdr),
	};
	struct spi_buf_set rx = {
		.buffers = &buf,
		.count = 1,
	};
	uint16_t len;
	int ret;

	ret = spi_read_dt(&cfg->bus, &rx);
	if (ret < 0) {
		LOG_ERR("Failed to receive header (%d)", ret);
		return;
	}

	LOG_HEXDUMP_DBG(hdr, sizeof(hdr), "HDLC SPI header");

	len = sys_get_le16(&hdr[3]);
	if (len > sizeof(data->rx_buf)) {
		LOG_ERR("Frame overflow %u > %zu", len, sizeof(data->rx_buf));
		// TODO: reset?
		return;
	}

	buf.buf = &data->rx_buf[0];
	buf.len = len;

	ret = spi_read_dt(&cfg->bus, &rx);
	if (ret < 0) {
		LOG_ERR("Failed to receive frame (%d)", ret);
		return;
	}

	if (data->rx_cb != NULL) {
		data->rx_cb(&data->rx_buf[0], len, data->rx_param);
	}
}

static void hdlc_rcp_if_spi_isr(const struct device *port, struct gpio_callback *cb,
				gpio_port_pins_t pins)
{
	struct hdlc_rcp_if_spi_data *data =
		CONTAINER_OF(cb, struct hdlc_rcp_if_spi_data, int_gpio_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_work_submit(&data->rx_work);
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
	const struct hdlc_rcp_if_spi_config *cfg = dev->config;
	uint8_t hdr[5];
	struct spi_buf bufs[] = {
		{
			.buf = hdr,
			.len = sizeof(hdr),
		},
		{
			.buf = (uint8_t *)frame,
			.len = length,
		},
	};
	struct spi_buf_set tx = {
		.buffers = bufs,
		.count = ARRAY_SIZE(bufs),
	};
	int ret;

	/* See https://datatracker.ietf.org/doc/html/draft-rquattle-spinel-unified#appendix-A.2.1 */
	hdr[0] = 0x02; /* HDR byte without CRC */
	sys_put_le16(0, &hdr[1]);
	sys_put_le16(length, &hdr[3]);

	ret = spi_write_dt(&cfg->bus, &tx);
	if (ret < 0) {
		LOG_ERR("Failed to send TX frame (%d)", ret);
		return ret;
	}

	return 0;
}

static int hdlc_deinit(void)
{
	return 0;
}

static int hdlc_rcp_if_spi_init(const struct device *dev)
{
	const struct hdlc_rcp_if_spi_config *cfg = dev->config;
	struct hdlc_rcp_if_spi_data *data = dev->data;
	int ret;

	data->self = dev;
	k_work_init(&data->rx_work, hdlc_rcp_if_spi_rx_handler);

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
};

static struct hdlc_rcp_if_spi_data ot_hdlc_rcp_data;

NET_DEVICE_DT_INST_DEFINE(0, hdlc_rcp_if_spi_init, NULL, &ot_hdlc_rcp_data, &ot_hdlc_rcp_cfg,
			  CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &spi_hdlc_api, OPENTHREAD_L2,
			  NET_L2_GET_CTX_TYPE(OPENTHREAD_L2), MTU);
