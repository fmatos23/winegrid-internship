/*
 * Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório - o padrão de ligação Wi-Fi replica (não copia) o já
 * usado em lwm2m/src/lwm2m-client.c, esse sim baseado num exemplo oficial.
 *
 * Zephyr + ThingsBoard PoC - Wi-Fi auto-connect (same pattern as
 * winegrid/lwm2m/src/lwm2m-client.c) followed by the ThingsBoard MQTT flow.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME zephyr_thingsboard
#define LOG_LEVEL LOG_LEVEL_INF

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <zephyr/kernel.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#if defined(CONFIG_WIFI)
#include <zephyr/net/wifi_mgmt.h>
#endif

#include "thingsboard_mqtt.h"

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;
static K_SEM_DEFINE(network_connected_sem, 0, 1);

static void l4_event_handler(struct net_mgmt_event_callback *cb, uint32_t event,
			      struct net_if *iface)
{
	if (event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("IP Up");
		k_sem_give(&network_connected_sem);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		LOG_INF("IP down");
	}
}

static void connectivity_event_handler(struct net_mgmt_event_callback *cb, uint32_t event,
					struct net_if *iface)
{
	if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
		LOG_ERR("Fatal error received from the connectivity layer");
	}
}

#if defined(CONFIG_WIFI)
static int wifi_auto_connect(struct net_if *iface)
{
	if (sizeof(CONFIG_APP_WIFI_SSID) <= 1) {
		return -ENOENT;
	}

	struct wifi_connect_req_params params = {
		.ssid = CONFIG_APP_WIFI_SSID,
		.ssid_length = sizeof(CONFIG_APP_WIFI_SSID) - 1,
		.psk = CONFIG_APP_WIFI_PSK,
		.psk_length = sizeof(CONFIG_APP_WIFI_PSK) - 1,
		.security = WIFI_SECURITY_TYPE_PSK,
		.band = WIFI_FREQ_BAND_UNKNOWN,
		.channel = WIFI_CHANNEL_ANY,
		.timeout = SYS_FOREVER_MS,
	};

	LOG_INF("Auto-connecting to Wi-Fi SSID '%s'", CONFIG_APP_WIFI_SSID);
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}
#endif /* CONFIG_WIFI */

int main(void)
{
	bool wait_for_network = false;
	int ret;

	LOG_INF("Zephyr + ThingsBoard PoC");

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);
	net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
	net_mgmt_add_event_callback(&conn_cb);

	ret = net_if_up(net_if_get_default());
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("net_if_up, error: %d", ret);
		return ret;
	}

#if defined(CONFIG_WIFI)
	ret = wifi_auto_connect(net_if_get_default());
	if (ret == 0) {
		wait_for_network = true;
	} else if (ret != -ENOENT) {
		LOG_ERR("Wi-Fi auto-connect request failed: %d", ret);
	}
#endif

	ret = conn_mgr_if_connect(net_if_get_default());
	if (ret == 0) {
		wait_for_network = true;
	}

	if (wait_for_network) {
		LOG_INF("Connecting to network");
		k_sem_take(&network_connected_sem, K_FOREVER);
	}

	thingsboard_run();

	LOG_ERR("thingsboard_run() returned - should never happen");
	return 0;
}