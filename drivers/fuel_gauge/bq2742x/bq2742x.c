/*
 * Copyright (c) 2025 Basalte bv
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_bq2742x

#include <zephyr/kernel.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bq2742x, CONFIG_FUEL_GAUGE_LOG_LEVEL);

struct bq2742x_config {
	struct i2c_dt_spec i2c;
};

static int bq2742x_get_prop(const struct device *dev, fuel_gauge_prop_t prop,
			    union fuel_gauge_prop_val *val)
{
	return -ENOTSUP;
}

static int bq2742x_get_buffer_prop(const struct device *dev, fuel_gauge_prop_t property_type,
				   void *dst, size_t dst_len)
{
	return -ENOTSUP;
}

static int bq2742x_set_prop(const struct device *dev, fuel_gauge_prop_t prop,
			    union fuel_gauge_prop_val val)
{
	return -ENOTSUP;
}

static int bq2742x_init(const struct device *dev)
{
	const struct bq2742x_config *cfg = dev->config;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("Bus device is not ready");
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(fuel_gauge, bq2742x_api) = {
	.get_property = bq2742x_get_prop,
	.set_property = bq2742x_set_prop,
	.get_buffer_property = bq2742x_get_buffer_prop,
};

#define BQ2742X_INIT(inst)                                                                         \
	static const struct bq2742x_config bq2742x_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &bq2742x_init, NULL, NULL, &bq2742x_config_##inst,             \
			      POST_KERNEL, CONFIG_FUEL_GAUGE_INIT_PRIORITY, &bq2742x_api);

DT_INST_FOREACH_STATUS_OKAY(BQ2742X_INIT)
