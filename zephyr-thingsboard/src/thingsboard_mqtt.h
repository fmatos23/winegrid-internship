#pragma once

/* Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório.
 *
 * Full ThingsBoard MQTT flow over the native Zephyr MQTT client: device
 * provisioning (if no token stored yet) -> connect with the issued access
 * token -> periodic telemetry -> RPC (getStatus/reboot). Blocks forever;
 * call once Wi-Fi/IP is up. Mirrors esp-idf-thingsboard/main/app_main.c. */
void thingsboard_run(void);