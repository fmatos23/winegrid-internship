/*
 * Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório - a lógica do chip vem da folha de dados pública do
 * MCP3221, o "molde" de driver Zephyr foi copiado da estrutura de
 * ficheiros reais (ver zephyr-drivers-referencia/), não do seu conteúdo.
 *
 * Copyright (c) 2026 Francisco Matos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Out-of-tree sensor driver for the Microchip MCP3221 single-channel
 * 12-bit I2C ADC. There is no in-tree Zephyr driver for this exact chip
 * (only the SPI-based MCP320x family is supported natively).
 */

#define DT_DRV_COMPAT microchip_mcp3221

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(MCP3221, CONFIG_SENSOR_LOG_LEVEL);

#define MCP3221_RESOLUTION_BITS 12

struct mcp3221_config {
	struct i2c_dt_spec bus;
	uint16_t vref_mv;
};

struct mcp3221_data {
	uint16_t raw;
};

static int mcp3221_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct mcp3221_config *config = dev->config;
	struct mcp3221_data *data = dev->data;
	uint8_t rx_buf[2];
	int err;

	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_VOLTAGE) {
		return -ENOTSUP;
	}

	/*
	 * The MCP3221 has no command/config registers: it converts
	 * continuously, and any plain I2C read transaction returns the
	 * 2 most recent conversion bytes (MSB first, top 4 bits of the
	 * first byte are always 0).
	 */
	err = i2c_read_dt(&config->bus, rx_buf, sizeof(rx_buf));
	if (err < 0) {
		LOG_ERR("failed to read MCP3221 (err %d)", err);
		return err;
	}

	data->raw = ((rx_buf[0] & 0x0F) << 8) | rx_buf[1];

	return 0;
}

static int mcp3221_channel_get(const struct device *dev, enum sensor_channel chan,
				struct sensor_value *val)
{
	const struct mcp3221_config *config = dev->config;
	struct mcp3221_data *data = dev->data;
	uint32_t microvolts;

	if (chan != SENSOR_CHAN_VOLTAGE) {
		return -ENOTSUP;
	}

	/* voltage = raw * Vref / 4096 (ratiometric - MCP3221 has no
	 * internal reference), kept in micro-units for precision.
	 */
	microvolts = ((uint32_t)data->raw * config->vref_mv * 1000U) /
		     BIT(MCP3221_RESOLUTION_BITS);

	val->val1 = microvolts / 1000000U;
	val->val2 = microvolts % 1000000U;

	return 0;
}

static int mcp3221_init(const struct device *dev)
{
	const struct mcp3221_config *config = dev->config;

	if (!i2c_is_ready_dt(&config->bus)) {
		LOG_ERR("I2C bus is not ready");
		return -ENODEV;
	}

	return 0;
}

static const struct sensor_driver_api mcp3221_driver_api = {
	.sample_fetch = mcp3221_sample_fetch,
	.channel_get = mcp3221_channel_get,
};

#define MCP3221_INIT(inst)						\
	static struct mcp3221_data mcp3221_data_##inst;		\
	static const struct mcp3221_config mcp3221_config_##inst = {	\
		.bus = I2C_DT_SPEC_INST_GET(inst),			\
		.vref_mv = DT_INST_PROP(inst, vref_millivolts),	\
	};								\
	SENSOR_DEVICE_DT_INST_DEFINE(inst, mcp3221_init, NULL,		\
			      &mcp3221_data_##inst,			\
			      &mcp3221_config_##inst, POST_KERNEL,	\
			      CONFIG_SENSOR_INIT_PRIORITY,		\
			      &mcp3221_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MCP3221_INIT)