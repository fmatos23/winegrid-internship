/*
 * Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório - a convenção de tópicos/payloads do ThingsBoard
 * replica (não copia) a mesma lógica já usada em
 * esp-idf-thingsboard/main/app_main.c.
 *
 * Zephyr + ThingsBoard PoC: native Zephyr MQTT client (subsys/net/lib/mqtt)
 * talking to the same ThingsBoard Cloud account/device profile validated
 * on ESP-IDF (esp-idf-thingsboard/main/app_main.c) - device provisioning,
 * telemetry, and RPC (getStatus/reboot). Unlike ESP RainMaker, ThingsBoard
 * is not tied to a firmware SDK: it only needs an MQTT client speaking its
 * topic/payload convention, which is exactly what this file provides.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

#include "thingsboard_mqtt.h"
#include "ca_cert.h"

LOG_MODULE_REGISTER(tb_mqtt, LOG_LEVEL_INF);

#define TB_PROVISION_TOPIC_REQUEST  "/provision/request"
#define TB_PROVISION_TOPIC_RESPONSE "/provision/response"
#define TB_TELEMETRY_TOPIC          "v1/devices/me/telemetry"
#define TB_RPC_REQUEST_TOPIC_FILTER "v1/devices/me/rpc/request/+"
#define TB_RPC_REQUEST_TOPIC_PREFIX "v1/devices/me/rpc/request/"

#define TB_CA_CERT_TAG 1

#define RX_BUF_SIZE 1024
#define TX_BUF_SIZE 1024
#define PAYLOAD_BUF_SIZE 512

static uint8_t rx_buffer[RX_BUF_SIZE];
static uint8_t tx_buffer[TX_BUF_SIZE];
static uint8_t payload_buf[PAYLOAD_BUF_SIZE];

static struct sockaddr_storage broker_addr;
static struct pollfd fds[1];

static const sec_tag_t sec_tags[] = { TB_CA_CERT_TAG };

/* client_configure() is only ever used sequentially (provisioning fully
 * disconnects before the main-phase client connects), so one static
 * mqtt_utf8 - whose lifetime must outlive the function that sets it, unlike
 * a stack compound literal - is safely reused for both. */
static struct mqtt_utf8 s_username;

static int s_fake_temperature = 20;

static int tls_init(void)
{
	int err = tls_credential_add(TB_CA_CERT_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				      tb_ca_certificate, sizeof(tb_ca_certificate));
	if (err < 0) {
		LOG_ERR("Failed to register CA certificate: %d", err);
	}
	return err;
}

static int resolve_broker(void)
{
	struct addrinfo *result;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};

	int err = getaddrinfo(CONFIG_TB_BROKER_HOSTNAME, CONFIG_TB_BROKER_PORT, &hints, &result);
	if (err != 0 || result == NULL) {
		LOG_ERR("Failed to resolve %s: %d", CONFIG_TB_BROKER_HOSTNAME, err);
		return -EIO;
	}

	memset(&broker_addr, 0, sizeof(broker_addr));
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker_addr;

	broker4->sin_family = AF_INET;
	broker4->sin_addr.s_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	broker4->sin_port = ((struct sockaddr_in *)result->ai_addr)->sin_port;
	freeaddrinfo(result);

	LOG_INF("Resolved %s", CONFIG_TB_BROKER_HOSTNAME);
	return 0;
}

static void get_device_name(char *out, size_t out_size)
{
	struct net_linkaddr *link_addr = net_if_get_link_addr(net_if_get_default());

	if (link_addr != NULL && link_addr->len == 6) {
		snprintf(out, out_size, "zephyr-%02x%02x%02x%02x%02x%02x",
			 link_addr->addr[0], link_addr->addr[1], link_addr->addr[2],
			 link_addr->addr[3], link_addr->addr[4], link_addr->addr[5]);
	} else {
		snprintf(out, out_size, "zephyr-unknown");
	}
}

/* Small ad hoc JSON string-value extractor for the two flat, known-shape
 * responses this file needs to parse ({"status":..,"credentialsValue":..}
 * and {"method":..}) - avoids pulling in Zephyr's schema-based JSON library
 * for what is otherwise a couple of fixed lookups. */
static bool json_extract_string(const char *json, size_t json_len, const char *key, char *out,
				 size_t out_size)
{
	char pattern[40];
	int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

	if (pattern_len <= 0 || (size_t)pattern_len >= json_len) {
		return false;
	}

	for (size_t i = 0; i + (size_t)pattern_len < json_len; i++) {
		if (memcmp(json + i, pattern, pattern_len) == 0) {
			size_t start = i + pattern_len;
			size_t end = start;

			while (end < json_len && json[end] != '"') {
				end++;
			}
			size_t len = end - start;

			if (len >= out_size) {
				len = out_size - 1;
			}
			memcpy(out, json + start, len);
			out[len] = '\0';
			return true;
		}
	}
	return false;
}

static void client_configure(struct mqtt_client *client, const char *username)
{
	mqtt_client_init(client);

	client->broker = &broker_addr;
	client->client_id.utf8 = (uint8_t *)"zephyr-tb-client";
	client->client_id.size = strlen("zephyr-tb-client");
	client->password = NULL;
	s_username.utf8 = (uint8_t *)username;
	s_username.size = strlen(username);
	client->user_name = &s_username;
	client->protocol_version = MQTT_VERSION_3_1_1;
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	client->transport.type = MQTT_TRANSPORT_SECURE;
	struct mqtt_sec_config *tls_config = &client->transport.tls.config;

	tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls_config->cipher_list = NULL;
	tls_config->sec_tag_list = sec_tags;
	tls_config->sec_tag_count = ARRAY_SIZE(sec_tags);
	tls_config->hostname = CONFIG_TB_BROKER_HOSTNAME;
}

static int mqtt_socket_fd(struct mqtt_client *client)
{
	return client->transport.tls.sock;
}

static int wait_socket(struct mqtt_client *client, int timeout_ms)
{
	fds[0].fd = mqtt_socket_fd(client);
	fds[0].events = POLLIN;
	return poll(fds, 1, timeout_ms);
}

/* --- Provisioning phase --- */

static bool s_provisioning_done;
static char s_provisioned_token[64];

static void provisioning_evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK: {
		if (evt->result != 0) {
			LOG_ERR("Provisioning CONNACK error: %d", evt->result);
			s_provisioning_done = true;
			break;
		}
		LOG_INF("Provisioning: connected, requesting credentials");

		struct mqtt_topic sub_topic = {
			.topic = { .utf8 = (uint8_t *)TB_PROVISION_TOPIC_RESPONSE,
				   .size = strlen(TB_PROVISION_TOPIC_RESPONSE) },
			.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		};
		struct mqtt_subscription_list sub_list = {
			.list = &sub_topic, .list_count = 1, .message_id = 1
		};
		mqtt_subscribe(client, &sub_list);

		char device_name[48];
		get_device_name(device_name, sizeof(device_name));

		char payload[256];
		int len = snprintf(payload, sizeof(payload),
				    "{\"deviceName\":\"%s\",\"provisionDeviceKey\":\"%s\","
				    "\"provisionDeviceSecret\":\"%s\"}",
				    device_name, CONFIG_TB_PROVISION_KEY, CONFIG_TB_PROVISION_SECRET);

		struct mqtt_publish_param pub = {
			.message.topic.topic.utf8 = (uint8_t *)TB_PROVISION_TOPIC_REQUEST,
			.message.topic.topic.size = strlen(TB_PROVISION_TOPIC_REQUEST),
			.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE,
			.message.payload.data = (uint8_t *)payload,
			.message.payload.len = len,
			.message_id = 2,
		};
		mqtt_publish(client, &pub);
		break;
	}
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;
		uint32_t len = pub->message.payload.len;

		if (len > sizeof(payload_buf) - 1) {
			len = sizeof(payload_buf) - 1;
		}
		mqtt_read_publish_payload(client, payload_buf, len);
		payload_buf[len] = '\0';

		char status[32];

		if (json_extract_string((char *)payload_buf, len, "status", status, sizeof(status)) &&
		    strcmp(status, "SUCCESS") == 0 &&
		    json_extract_string((char *)payload_buf, len, "credentialsValue",
					 s_provisioned_token, sizeof(s_provisioned_token))) {
			LOG_INF("Provisioning succeeded, token issued");
		} else {
			LOG_ERR("Provisioning failed: %s", payload_buf);
		}
		s_provisioning_done = true;
		break;
	}
	case MQTT_EVT_DISCONNECT:
		LOG_INF("Provisioning: disconnected");
		break;
	default:
		break;
	}
}

static bool run_provisioning(char *out_token, size_t out_size)
{
	static struct mqtt_client client;

	client_configure(&client, "provision");
	client.evt_cb = provisioning_evt_handler;

	int err = mqtt_connect(&client);
	if (err != 0) {
		LOG_ERR("Provisioning mqtt_connect failed: %d", err);
		return false;
	}

	s_provisioning_done = false;
	int64_t deadline = k_uptime_get() + 15000;

	while (!s_provisioning_done && k_uptime_get() < deadline) {
		if (wait_socket(&client, 500) > 0) {
			mqtt_input(&client);
		}
		mqtt_live(&client);
	}

	mqtt_disconnect(&client);

	if (!s_provisioning_done || strlen(s_provisioned_token) == 0) {
		LOG_ERR("Provisioning timed out");
		return false;
	}

	strncpy(out_token, s_provisioned_token, out_size - 1);
	out_token[out_size - 1] = '\0';
	return true;
}

/* --- Main phase: telemetry + RPC --- */

static struct mqtt_client s_client;

static void handle_rpc_request(const char *topic, size_t topic_len, const char *data, size_t data_len)
{
	char response_topic[64];
	const char *request_id = topic + strlen(TB_RPC_REQUEST_TOPIC_PREFIX);
	int request_id_len = (int)topic_len - (int)strlen(TB_RPC_REQUEST_TOPIC_PREFIX);

	snprintf(response_topic, sizeof(response_topic), "v1/devices/me/rpc/response/%.*s",
		 request_id_len, request_id);

	char method[32];

	if (!json_extract_string(data, data_len, "method", method, sizeof(method))) {
		return;
	}

	LOG_INF("RPC request: method=%s", method);

	char resp_payload[128];
	int resp_len;

	if (strcmp(method, "getStatus") == 0) {
		resp_len = snprintf(resp_payload, sizeof(resp_payload),
				     "{\"temperature\":%d,\"uptimeMs\":%lld}",
				     s_fake_temperature, k_uptime_get());
	} else if (strcmp(method, "reboot") == 0) {
		resp_len = snprintf(resp_payload, sizeof(resp_payload), "{\"result\":\"rebooting\"}");
	} else {
		resp_len = snprintf(resp_payload, sizeof(resp_payload), "{\"error\":\"unknown method\"}");
	}

	struct mqtt_publish_param pub = {
		.message.topic.topic.utf8 = (uint8_t *)response_topic,
		.message.topic.topic.size = strlen(response_topic),
		.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.message.payload.data = (uint8_t *)resp_payload,
		.message.payload.len = resp_len,
		.message_id = 100,
	};
	mqtt_publish(&s_client, &pub);

	if (strcmp(method, "reboot") == 0) {
		k_sleep(K_MSEC(500));
		sys_reboot(SYS_REBOOT_WARM);
	}
}

static void main_evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("CONNACK error: %d", evt->result);
			break;
		}
		LOG_INF("Connected to ThingsBoard");

		struct mqtt_topic sub_topic = {
			.topic = { .utf8 = (uint8_t *)TB_RPC_REQUEST_TOPIC_FILTER,
				   .size = strlen(TB_RPC_REQUEST_TOPIC_FILTER) },
			.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		};
		struct mqtt_subscription_list sub_list = {
			.list = &sub_topic, .list_count = 1, .message_id = 1
		};
		mqtt_subscribe(client, &sub_list);
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;
		uint32_t len = pub->message.payload.len;
		const char *topic = (char *)pub->message.topic.topic.utf8;
		size_t topic_len = pub->message.topic.topic.size;

		if (len > sizeof(payload_buf) - 1) {
			len = sizeof(payload_buf) - 1;
		}
		mqtt_read_publish_payload(client, payload_buf, len);
		payload_buf[len] = '\0';

		if (topic_len >= strlen(TB_RPC_REQUEST_TOPIC_PREFIX) &&
		    strncmp(topic, TB_RPC_REQUEST_TOPIC_PREFIX, strlen(TB_RPC_REQUEST_TOPIC_PREFIX)) == 0) {
			handle_rpc_request(topic, topic_len, (char *)payload_buf, len);
		}
		break;
	}

	case MQTT_EVT_DISCONNECT:
		LOG_INF("Disconnected from ThingsBoard");
		break;

	default:
		break;
	}
}

static void publish_telemetry(struct mqtt_client *client)
{
	s_fake_temperature = 20 + (s_fake_temperature + 1 - 20) % 10;

	char payload[64];
	int len = snprintf(payload, sizeof(payload), "{\"temperature\":%d}", s_fake_temperature);

	struct mqtt_publish_param pub = {
		.message.topic.topic.utf8 = (uint8_t *)TB_TELEMETRY_TOPIC,
		.message.topic.topic.size = strlen(TB_TELEMETRY_TOPIC),
		.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.message.payload.data = (uint8_t *)payload,
		.message.payload.len = len,
		.message_id = 3,
	};
	int err = mqtt_publish(client, &pub);

	if (err == 0) {
		LOG_INF("Telemetry published: %s", payload);
	} else {
		LOG_ERR("Telemetry publish failed: %d", err);
	}
}

void thingsboard_run(void)
{
	if (tls_init() < 0) {
		return;
	}
	if (resolve_broker() < 0) {
		return;
	}

	char access_token[64];

	LOG_INF("Starting device provisioning");
	if (!run_provisioning(access_token, sizeof(access_token))) {
		LOG_ERR("Provisioning failed, aborting");
		return;
	}

	client_configure(&s_client, access_token);
	s_client.evt_cb = main_evt_handler;

	int err = mqtt_connect(&s_client);
	if (err != 0) {
		LOG_ERR("mqtt_connect failed: %d", err);
		return;
	}

	int64_t next_telemetry = k_uptime_get();

	while (true) {
		if (wait_socket(&s_client, 1000) > 0) {
			mqtt_input(&s_client);
		}
		mqtt_live(&s_client);

		if (k_uptime_get() >= next_telemetry) {
			publish_telemetry(&s_client);
			next_telemetry = k_uptime_get() + 10000;
		}
	}
}