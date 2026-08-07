/*
 * Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório.
 *
 * Prova de conceito do componente MCP3221 em ESP-IDF (equivalente ao
 * driver out-of-tree escrito para o Zephyr). Sem o chip físico ligado,
 * esta aplicação valida que o componente compila e é chamado
 * corretamente - não valida a leitura real do ADC.
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "mcp3221.h"

static const char *TAG = "app_main";

/* Mesmos pinos usados por omissão na board esp32_devkitc_wroom no Zephyr,
 * para manter a comparação equivalente entre as duas plataformas. */
#define I2C_SDA_GPIO 21
#define I2C_SCL_GPIO 22
#define MCP3221_I2C_ADDR 0x4D

void app_main(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    mcp3221_config_t mcp_config = {
        .i2c_address = MCP3221_I2C_ADDR,
        .vref_mv = 3300,
    };

    mcp3221_handle_t mcp3221;
    esp_err_t err = mcp3221_init(bus, &mcp_config, &mcp3221);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha a inicializar o MCP3221 (%s)", esp_err_to_name(err));
        return;
    }

    while (1) {
        uint32_t millivolts;
        err = mcp3221_read_voltage_mv(mcp3221, &millivolts);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "MCP3221 tensao: %" PRIu32 " mV", millivolts);
        } else {
            ESP_LOGW(TAG, "leitura falhou (esperado sem hardware fisico ligado): %s",
                      esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}