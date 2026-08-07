# Winegrid — Avaliação de Plataformas de Gestão de Dispositivos ESP32

Estágio na Watgrid: avaliação de sistemas operativos/plataformas de firmware (Zephyr, ESP-IDF, Mongoose OS) e de plataformas de gestão de dispositivos na nuvem (ThingsBoard, ESP RainMaker, Azure IoT Hub + DPS) para o hardware ESP32.

**Relatórios:**
- [`relatorio.pdf`](relatorio.pdf) — relatório completo, com todo o processo de investigação e diagnóstico.
- [`relatorioConciso.pdf`](relatorioConciso.pdf) — versão condensada, só com resultados e conclusões.

## Camadas de firmware

| Pasta | O que é |
|---|---|
| [`lwm2m/`](lwm2m/) | Cliente LwM2M em Zephyr (baseado no exemplo oficial `samples/net/lwm2m_client`), com OTA real sobre Wi-Fi via Object 5 (Firmware Update) — inclui o diagnóstico e correção do bug crítico de OTA do Zephyr 3.7.0. |
| [`smp_svr/`](smp_svr/) | Exemplo oficial do Zephyr (MCUboot + mcumgr, OTA por série), não modificado. |
| [`esp-idf-ota/`](esp-idf-ota/) | Exemplo oficial do ESP-IDF (`native_ota_example`), usado para validar OTA nativo (HTTP) e *rollback* automático. |
| [`zephyr-drivers-referencia/`](zephyr-drivers-referencia/) | Cópias de ficheiros reais do código-fonte do Zephyr (drivers IIS2ICLX, LoRa, MCP320x) usadas como referência — ver [`ORIGEM.md`](zephyr-drivers-referencia/ORIGEM.md) para a proveniência exata de cada ficheiro. |
| [`zephyr-mcp3221-poc/`](zephyr-mcp3221-poc/) | Prova de conceito: driver *out-of-tree* escrito de raiz para o sensor MCP3221 (ADC I2C) no Zephyr, sem suporte nativo. |
| [`esp-idf-mcp3221-poc/`](esp-idf-mcp3221-poc/) | O mesmo driver MCP3221, agora como componente ESP-IDF — para comparar o esforço de integração entre os dois frameworks. |
| [`zephyr-rn2483-poc/`](zephyr-rn2483-poc/) | Prova de conceito: driver *out-of-tree* para o módulo LoRa RN2483 (comandos de texto via UART) no Zephyr. |
| [`mongoose-os-demo-c/`](mongoose-os-demo-c/) | Clone oficial do exemplo `demo-c` do Mongoose OS, usado para testar se a instalação/compilação/gravação ainda funcionam hoje (funcionam, apesar do projeto estar sem manutenção real desde 2023). |

## Plataformas de gestão (cloud)

| Pasta | O que é |
|---|---|
| [`esp-idf-thingsboard/`](esp-idf-thingsboard/) | Cliente MQTT para o ThingsBoard Cloud em ESP-IDF (baseado no exemplo oficial `mqtt/tcp`, muito estendido): provisionamento automático, telemetria, RPC, OTA, MQTTS/TLS. |
| [`zephyr-thingsboard/`](zephyr-thingsboard/) | O mesmo fluxo ThingsBoard, agora em Zephyr, sobre o cliente MQTT nativo do próprio Zephyr — confirma que o ThingsBoard não exige nenhum SDK de firmware específico (ao contrário do RainMaker). |
| [`esp-idf-azure-dps/`](esp-idf-azure-dps/) | PoC do Azure IoT Hub + Device Provisioning Service (DPS): SAS tokens, Direct Methods, Device Twin, OTA, C2D, *Group Enrollment*. |
| `esp-rainmaker/` | **Não incluído neste repositório** (ver `.gitignore`) — clone completo do SDK oficial do ESP RainMaker, usado localmente para os testes documentados no relatório; só o exemplo `temperature_sensor` foi modificado. |

## Scripts

| Ficheiro | Para que serve |
|---|---|
| [`esp32_ota_test.sh`](esp32_ota_test.sh) | Compila, grava e testa MCUboot + smp_svr (OTA por mcumgr/série). |
| [`esp_idf_ota_test.sh`](esp_idf_ota_test.sh) | Compila, grava e testa o OTA nativo do ESP-IDF (HTTP). |
| [`lwm2m_test.sh`](lwm2m_test.sh) | Compila e grava o cliente LwM2M (Wi-Fi). |
| [`ota_update.py`](ota_update.py) | Envia uma imagem de firmware assinada por mcumgr/série. |
| [`lwm2m_ota_push.py`](lwm2m_ota_push.py) | Envia uma imagem de firmware por CoAP diretamente ao Object 5 do LwM2M (mais fiável que a API REST do Leshan para binários grandes). |

## Notas sobre origem do código

Todos os ficheiros de código têm, no topo, uma nota a indicar se foram escritos de raiz nesta colaboração, se são baseados num exemplo oficial (e o que foi alterado), ou se são cópias inalteradas — ver o próprio ficheiro para o detalhe exato de cada caso.
