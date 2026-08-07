# Origem do código

Todos os ficheiros nesta pasta são cópias diretas do repositório oficial do Zephyr RTOS, extraídas da checkout local em `/home/francisco/zephyr` (a mesma usada nos testes de Zephyr do relatório).

- **Repositório:** https://github.com/zephyrproject-rtos/zephyr
- **Versão/tag:** `v3.7.0`
- **Commit exato:** `36940db938a8f4a1e919496793ed439850a221c2`
- **Licença:** Apache-2.0 (indicada via `SPDX-License-Identifier` no topo de cada ficheiro)

Nenhum ficheiro foi escrito por mim — são código-fonte nativo do Zephyr, copiados tal e qual para consulta local, sem ligação à internet necessária. Os links abaixo apontam para o mesmo ficheiro, na mesma tag, no GitHub.

## `iis2iclx/`
Driver nativo do acelerómetro/inclinómetro ST IIS2ICLX (I2C + SPI). Autoria original: **STMicroelectronics**.

| Ficheiro local | Origem (GitHub, tag v3.7.0) |
|---|---|
| `iis2iclx.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/sensor/st/iis2iclx/iis2iclx.c |
| `iis2iclx.h` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/sensor/st/iis2iclx/iis2iclx.h |
| `iis2iclx_trigger.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/sensor/st/iis2iclx/iis2iclx_trigger.c |
| `iis2iclx_shub.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/sensor/st/iis2iclx/iis2iclx_shub.c |
| `Kconfig` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/sensor/st/iis2iclx/Kconfig |
| `CMakeLists.txt` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/sensor/st/iis2iclx/CMakeLists.txt |
| `st,iis2iclx-common.yaml` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/dts/bindings/sensor/st,iis2iclx-common.yaml |
| `st,iis2iclx-i2c.yaml` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/dts/bindings/sensor/st,iis2iclx-i2c.yaml |
| `st,iis2iclx-spi.yaml` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/dts/bindings/sensor/st,iis2iclx-spi.yaml |
| `iis2iclx.h` (dt-bindings) | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/include/zephyr/dt-bindings/sensor/iis2iclx.h |

## `lora/`
Subsistema nativo de LoRa (Semtech SX126x/SX127x + módulos AT RYLR). Vários autores, por ficheiro:

| Ficheiro local | Origem (GitHub, tag v3.7.0) | Autoria original |
|---|---|---|
| `sx126x.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/sx126x.c | Andreas Sandberg (2020) |
| `sx126x_common.h` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/sx126x_common.h | Andreas Sandberg (2020) |
| `sx126x_standalone.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/sx126x_standalone.c | Andreas Sandberg (2020) |
| `sx126x_stm32wl.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/sx126x_stm32wl.c | Fabio Baltieri (2021) |
| `sx127x.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/sx127x.c | Manivannan Sadhasivam (2019) |
| `sx12xx_common.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/sx12xx_common.c | Manivannan Sadhasivam (2019) |
| `sx12xx_common.h` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/sx12xx_common.h | Manivannan Sadhasivam (2019) |
| `rylrxxx.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/rylrxxx.c | David Ullmann (2024) |
| `hal_common.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/hal_common.c | Grinn (2020) |
| `shell.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/shell.c | Manivannan Sadhasivam (2019) |
| `Kconfig` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/Kconfig | Manivannan Sadhasivam (2019) |
| `Kconfig.sx12xx` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/Kconfig.sx12xx | Manivannan Sadhasivam (2019) |
| `Kconfig.rylrxxx` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/Kconfig.rylrxxx | David Ullmann (2024) |
| `CMakeLists.txt` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/lora/CMakeLists.txt | Manivannan Sadhasivam (2019) |

## `adc_mcp320x_referencia_spi/`
Driver da família MCP3204/MCP3208 (ADC **SPI**, não é o MCP3221 pedido, que é I2C) -- mantido apenas como referência, por ser o chip mais próximo com driver nativo encontrado no Zephyr. Autoria original: **Vestas Wind Systems A/S** (2020).

| Ficheiro local | Origem (GitHub, tag v3.7.0) |
|---|---|
| `adc_mcp320x.c` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/adc/adc_mcp320x.c |
| `Kconfig.mcp320x` | https://github.com/zephyrproject-rtos/zephyr/blob/v3.7.0/drivers/adc/Kconfig.mcp320x |

## Nota sobre o ESP-IDF
Não há pasta equivalente para ESP-IDF porque a investigação (secção 4.5 do relatório) confirmou que não existe, nem no repositório principal nem no ESP Component Registry (`components.espressif.com`), nenhum driver nativo ou de terceiros publicado para IIS2ICLX, KX122 ou MCP3221 -- não havia código para copiar. O suporte a LoRa no ESP-IDF existe apenas como componente de terceiros (`jgromes/radiolib`, `dernasherbrezon/sx127x`), fora deste repositório.