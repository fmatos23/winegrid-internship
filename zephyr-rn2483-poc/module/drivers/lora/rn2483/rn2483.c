/*
 * Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório - o protocolo de comandos vem da documentação
 * pública da Microchip para o RN2483, o "molde" de driver Zephyr foi
 * copiado da estrutura de ficheiros reais (ver zephyr-drivers-referencia/
 * e rylrxxx.c em particular), não do seu conteúdo.
 *
 * Copyright (c) 2026 Francisco Matos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Out-of-tree driver for the Microchip RN2483 LoRa module.
 *
 * Unlike bare Semtech transceivers (SX126x/SX127x, natively supported by
 * Zephyr's LoRa subsystem via SPI), the RN2483 has its own onboard MCU and
 * is controlled entirely through plain-text commands over UART (57600
 * 8N1) - closer in spirit to an AT-command modem than to a radio register
 * driver. This plugs into the same zephyr/drivers/lora.h API
 * (lora_config()/lora_send()) as any native LoRa driver, using simple
 * polled UART I/O rather than Zephyr's more elaborate modem-chat subsystem
 * (used by the in-tree drivers/lora/rylrxxx.c for a different AT-command
 * module), to keep this proof of concept minimal.
 */

#define DT_DRV_COMPAT microchip_rn2483

#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

LOG_MODULE_REGISTER(rn2483, CONFIG_LORA_LOG_LEVEL);

#define RN2483_RESP_BUF_SIZE   64
#define RN2483_MAX_PAYLOAD     64
#define RN2483_CMD_TIMEOUT_MS  2000
#define RN2483_TX_TIMEOUT_MS   8000

struct rn2483_config {
	const struct device *uart;
};

struct rn2483_data {
	char resp_buf[RN2483_RESP_BUF_SIZE];
};

static void rn2483_write_line(const struct device *dev, const char *cmd, size_t cmd_len)
{
	const struct rn2483_config *config = dev->config;

	for (size_t i = 0; i < cmd_len; i++) {
		uart_poll_out(config->uart, cmd[i]);
	}
}

/* Reads characters until a full line (terminated by '\n') is received or
 * the timeout expires. '\r' is stripped. Returns the line length (>= 0) or
 * -ETIMEDOUT.
 */
static int rn2483_read_line(const struct device *dev, int timeout_ms)
{
	const struct rn2483_config *config = dev->config;
	struct rn2483_data *data = dev->data;
	size_t pos = 0;
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (k_uptime_get() < deadline) {
		unsigned char c;

		if (uart_poll_in(config->uart, &c) == 0) {
			if (c == '\n') {
				data->resp_buf[pos] = '\0';
				return (int)pos;
			}
			if (c != '\r' && pos < RN2483_RESP_BUF_SIZE - 1) {
				data->resp_buf[pos++] = (char)c;
			}
		} else {
			k_sleep(K_MSEC(2));
		}
	}

	return -ETIMEDOUT;
}

static int rn2483_cmd_expect_ok(const struct device *dev, const char *cmd, size_t cmd_len)
{
	struct rn2483_data *data = dev->data;
	int ret;

	rn2483_write_line(dev, cmd, cmd_len);
	ret = rn2483_read_line(dev, RN2483_CMD_TIMEOUT_MS);
	if (ret < 0) {
		return ret;
	}
	if (strcmp(data->resp_buf, "ok") != 0) {
		LOG_ERR("unexpected response: %s", data->resp_buf);
		return -EIO;
	}
	return 0;
}

static uint32_t rn2483_bandwidth_khz(enum lora_signal_bandwidth bw)
{
	switch (bw) {
	case BW_125_KHZ:
		return 125;
	case BW_250_KHZ:
		return 250;
	case BW_500_KHZ:
		return 500;
	default:
		return 125;
	}
}

static int rn2483_config(const struct device *dev, struct lora_modem_config *config)
{
	char cmd[48];
	int len;
	int err;

	len = snprintf(cmd, sizeof(cmd), "radio set freq %u\r\n", config->frequency);
	err = rn2483_cmd_expect_ok(dev, cmd, len);
	if (err) {
		return err;
	}

	len = snprintf(cmd, sizeof(cmd), "radio set pwr %d\r\n", config->tx_power);
	err = rn2483_cmd_expect_ok(dev, cmd, len);
	if (err) {
		return err;
	}

	len = snprintf(cmd, sizeof(cmd), "radio set sf sf%u\r\n", config->datarate);
	err = rn2483_cmd_expect_ok(dev, cmd, len);
	if (err) {
		return err;
	}

	len = snprintf(cmd, sizeof(cmd), "radio set bw %u\r\n",
		       rn2483_bandwidth_khz(config->bandwidth));
	return rn2483_cmd_expect_ok(dev, cmd, len);
}

static int rn2483_send(const struct device *dev, uint8_t *data_buf, uint32_t data_len)
{
	struct rn2483_data *data = dev->data;
	char cmd[2 * RN2483_MAX_PAYLOAD + 16];
	int pos;
	int err;

	if (data_len > RN2483_MAX_PAYLOAD) {
		return -EINVAL;
	}

	pos = snprintf(cmd, sizeof(cmd), "radio tx ");
	for (uint32_t i = 0; i < data_len; i++) {
		pos += snprintf(cmd + pos, sizeof(cmd) - pos, "%02x", data_buf[i]);
	}
	pos += snprintf(cmd + pos, sizeof(cmd) - pos, "\r\n");

	err = rn2483_cmd_expect_ok(dev, cmd, pos);
	if (err) {
		return err;
	}

	/* The module acknowledges the command immediately ("ok"), but actual
	 * over-the-air completion arrives later, asynchronously, as its own
	 * line ("radio_tx_ok" or "radio_err ...").
	 */
	err = rn2483_read_line(dev, RN2483_TX_TIMEOUT_MS);
	if (err < 0) {
		return err;
	}
	if (strcmp(data->resp_buf, "radio_tx_ok") == 0) {
		return 0;
	}

	LOG_ERR("tx failed: %s", data->resp_buf);
	return -EIO;
}

static int rn2483_send_async_stub(const struct device *dev, uint8_t *data_buf,
				   uint32_t data_len, struct k_poll_signal *async)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data_buf);
	ARG_UNUSED(data_len);
	ARG_UNUSED(async);
	return -ENOSYS;
}

static int rn2483_recv_stub(const struct device *dev, uint8_t *data_buf, uint8_t size,
			     k_timeout_t timeout, int16_t *rssi, int8_t *snr)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data_buf);
	ARG_UNUSED(size);
	ARG_UNUSED(timeout);
	ARG_UNUSED(rssi);
	ARG_UNUSED(snr);
	return -ENOSYS;
}

static int rn2483_recv_async_stub(const struct device *dev, lora_recv_cb cb)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	return -ENOSYS;
}

static int rn2483_test_cw_stub(const struct device *dev, uint32_t frequency,
				int8_t tx_power, uint16_t duration)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(frequency);
	ARG_UNUSED(tx_power);
	ARG_UNUSED(duration);
	return -ENOSYS;
}

static int rn2483_init(const struct device *dev)
{
	const struct rn2483_config *config = dev->config;
	struct rn2483_data *data = dev->data;
	unsigned char c;
	int ret;

	if (!device_is_ready(config->uart)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	/* Drain any boot-time banner the module may already have sent. */
	while (uart_poll_in(config->uart, &c) == 0) {
		/* discard */
	}

	rn2483_write_line(dev, "sys get ver\r\n", strlen("sys get ver\r\n"));
	ret = rn2483_read_line(dev, RN2483_CMD_TIMEOUT_MS);
	if (ret < 0) {
		LOG_ERR("no response from RN2483 (sys get ver)");
		return ret;
	}

	LOG_INF("RN2483 firmware version: %s", data->resp_buf);
	return 0;
}

static const struct lora_driver_api rn2483_driver_api = {
	.config = rn2483_config,
	.send = rn2483_send,
	.send_async = rn2483_send_async_stub,
	.recv = rn2483_recv_stub,
	.recv_async = rn2483_recv_async_stub,
	.test_cw = rn2483_test_cw_stub,
};

#define RN2483_INIT(inst)						\
	static struct rn2483_data rn2483_data_##inst;			\
	static const struct rn2483_config rn2483_config_##inst = {	\
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),		\
	};								\
	DEVICE_DT_INST_DEFINE(inst, rn2483_init, NULL,			\
			      &rn2483_data_##inst,			\
			      &rn2483_config_##inst, POST_KERNEL,	\
			      CONFIG_LORA_INIT_PRIORITY,		\
			      &rn2483_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RN2483_INIT)