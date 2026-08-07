/*
 * Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório.
 *
 * Componente ESP-IDF para o MCP3221 (ADC I2C de 12 bits, canal único da
 * Microchip). Não existe suporte oficial para este chip nem no core do
 * ESP-IDF nem no ESP Component Registry - este componente foi escrito
 * de raiz como prova de conceito, equivalente ao driver out-of-tree
 * feito para o Zephyr (ver zephyr-mcp3221-poc/).
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp3221_dev *mcp3221_handle_t;

typedef struct {
    uint16_t i2c_address; /* endereço fixo de fábrica, ex: 0x4D (MCP3221A5T-E/OT) */
    uint16_t vref_mv;     /* Vref = Vdd (sem referência interna); tipicamente 3300 no ESP32 */
} mcp3221_config_t;

/* Regista o dispositivo no barramento I2C já inicializado. */
esp_err_t mcp3221_init(i2c_master_bus_handle_t bus, const mcp3221_config_t *config,
                        mcp3221_handle_t *out_handle);

/* Lê o valor bruto de 12 bits (leitura I2C direta, sem registos de configuração). */
esp_err_t mcp3221_read_raw(mcp3221_handle_t handle, uint16_t *raw12);

/* Lê e converte para milivolts (raw * Vref / 4096). */
esp_err_t mcp3221_read_voltage_mv(mcp3221_handle_t handle, uint32_t *millivolts);

void mcp3221_deinit(mcp3221_handle_t handle);

#ifdef __cplusplus
}
#endif