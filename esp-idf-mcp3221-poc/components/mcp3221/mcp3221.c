/* Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório.
 */
#include "mcp3221.h"
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "mcp3221";

#define MCP3221_RESOLUTION_BITS 12

struct mcp3221_dev {
    i2c_master_dev_handle_t i2c_dev;
    uint16_t vref_mv;
};

esp_err_t mcp3221_init(i2c_master_bus_handle_t bus, const mcp3221_config_t *config,
                        mcp3221_handle_t *out_handle)
{
    if (!bus || !config || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct mcp3221_dev *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->i2c_address,
        .scl_speed_hz = 100000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }

    dev->vref_mv = config->vref_mv;
    *out_handle = dev;
    return ESP_OK;
}

esp_err_t mcp3221_read_raw(mcp3221_handle_t handle, uint16_t *raw12)
{
    if (!handle || !raw12) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t rx_buf[2];

    /* O MCP3221 não tem registos de configuração: qualquer transação de
     * leitura I2C devolve os 2 bytes mais recentes da conversão (MSB
     * primeiro, os 4 bits superiores do primeiro byte são sempre 0). */
    esp_err_t err = i2c_master_receive(handle->i2c_dev, rx_buf, sizeof(rx_buf), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao ler o MCP3221 (%s)", esp_err_to_name(err));
        return err;
    }

    *raw12 = ((rx_buf[0] & 0x0F) << 8) | rx_buf[1];
    return ESP_OK;
}

esp_err_t mcp3221_read_voltage_mv(mcp3221_handle_t handle, uint32_t *millivolts)
{
    if (!handle || !millivolts) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw;
    esp_err_t err = mcp3221_read_raw(handle, &raw);
    if (err != ESP_OK) {
        return err;
    }

    /* tensão = raw * Vref / 4096 (ratiométrico - sem referência interna) */
    *millivolts = ((uint32_t)raw * handle->vref_mv) / (1U << MCP3221_RESOLUTION_BITS);
    return ESP_OK;
}

void mcp3221_deinit(mcp3221_handle_t handle)
{
    if (handle) {
        i2c_master_bus_rm_device(handle->i2c_dev);
        free(handle);
    }
}