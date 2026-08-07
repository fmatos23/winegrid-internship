/* Origem: baseado no exemplo oficial do ESP-IDF v5.5.1
 * (examples/protocols/mqtt/tcp/main/app_main.c), que na forma original só
 * faz um demo simples de publish/subscribe. Estendido substancialmente
 * nesta colaboração (Claude): provisionamento automático, tópicos e
 * payloads específicos do ThingsBoard, RPC, OTA sobre MQTT, proteção
 * anti-loop, MQTTS/TLS - nada disto vem do exemplo original.
 *
 * MQTT (over TCP) Example - adapted to publish telemetry to ThingsBoard Cloud.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#include "esp_crt_bundle.h"
#include "nvs.h"

static const char *TAG = "tb_mqtt_example";

#define TB_PROVISION_TOPIC_REQUEST "/provision/request"
#define TB_PROVISION_TOPIC_RESPONSE "/provision/response"
#define NVS_NAMESPACE "tb_prov"
#define NVS_KEY_TOKEN "access_token"
#define NVS_KEY_PENDING_FW "pending_fw"
#define NVS_KEY_FAILED_FW "failed_fw"

#define TB_TELEMETRY_TOPIC "v1/devices/me/telemetry"
#define TB_ATTRIBUTES_TOPIC "v1/devices/me/attributes"
#define TB_ATTRIBUTES_REQUEST_TOPIC "v1/devices/me/attributes/request/1"
#define TB_ATTRIBUTES_RESPONSE_TOPIC "v1/devices/me/attributes/response/+"
#define TB_FW_RESPONSE_TOPIC_FILTER "v2/fw/response/+/chunk/+"
#define TB_RPC_REQUEST_TOPIC_FILTER "v1/devices/me/rpc/request/+"
#define TB_RPC_REQUEST_TOPIC_PREFIX "v1/devices/me/rpc/request/"
#define TB_DEFAULT_PUBLISH_INTERVAL_MS 10000
#define TB_FW_CHUNK_SIZE 4096

/* Bump manually to demonstrate a new version being offered from the
 * ThingsBoard OTA repository - same spirit as version.txt in the ESP-IDF
 * native OTA PoC, just inlined here since this project doesn't need a
 * separate bump script. */
#define CURRENT_FW_TITLE "esp32-winegrid"
#define CURRENT_FW_VERSION "1.7"

static esp_mqtt_client_handle_t s_client;
static bool s_mqtt_connected;
/* Shared attribute pushed from ThingsBoard - overrides how often we publish
 * telemetry, without needing a rebuild/reflash (the remote-command
 * equivalent to LwM2M's Object 5 "Update" exec, but generic to any value
 * instead of just firmware). */
static int s_publish_interval_ms = TB_DEFAULT_PUBLISH_INTERVAL_MS;
/* Same fake sensor reading the telemetry loop publishes on its own timer -
 * kept as a global so the RPC handler can also read it on demand. */
static int s_fake_temperature = 20;

/* OTA state - protocol per ThingsBoard's reference mqtt_firmware_client.py:
 * shared attrs fw_title/fw_version/fw_size/fw_checksum/fw_checksum_algorithm
 * describe the package waiting in the OTA repository; chunks are pulled one
 * at a time over v2/fw/request|response, written straight to the inactive
 * OTA partition (never buffered whole in RAM - the binary is ~1MB, more
 * than the ESP32's free heap). */
static char s_fw_title[64];
static char s_fw_version[32];
static char s_fw_checksum[65];
static char s_fw_checksum_algorithm[16];
static int s_fw_size;
static bool s_fw_info_known;
static bool s_fw_update_in_progress;
static int s_fw_request_id;
static int s_fw_chunk_index;
static int s_fw_received;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_update_partition;
static mbedtls_sha256_context s_sha_ctx;

static void send_telemetry_json(const char *json)
{
    esp_mqtt_client_publish(s_client, TB_TELEMETRY_TOPIC, json, 0, 1, 0);
}

static void report_fw_state(const char *state)
{
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"fw_state\":\"%s\"}", state);
    send_telemetry_json(payload);
    ESP_LOGI(TAG, "fw_state -> %s", state);
}

/* Defined later in the file, alongside the other NVS helpers. */
static bool load_nvs_str(const char *key, char *out, size_t out_size);
static void save_nvs_str(const char *key, const char *value);

static void request_fw_chunk(void)
{
    char topic[64];
    snprintf(topic, sizeof(topic), "v2/fw/request/%d/chunk/%d", s_fw_request_id, s_fw_chunk_index);
    char payload[16];
    snprintf(payload, sizeof(payload), "%d", TB_FW_CHUNK_SIZE);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
}

static void start_fw_update(void)
{
    ESP_LOGW(TAG, "New firmware available: %s %s (%d bytes, %s checksum) - current: %s %s",
             s_fw_title, s_fw_version, s_fw_size, s_fw_checksum_algorithm,
             CURRENT_FW_TITLE, CURRENT_FW_VERSION);

    char failed_version[32];
    if (load_nvs_str(NVS_KEY_FAILED_FW, failed_version, sizeof(failed_version)) &&
        strcmp(failed_version, s_fw_version) == 0) {
        ESP_LOGE(TAG, "Firmware %s previously failed on this device - refusing to retry automatically "
                       "(assign a different version on ThingsBoard to retry)", s_fw_version);
        char payload[192];
        snprintf(payload, sizeof(payload),
                 "{\"fw_state\":\"FAILED\",\"fw_error\":\"Version %s previously failed - refusing automatic retry\"}",
                 s_fw_version);
        send_telemetry_json(payload);
        return;
    }

    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        report_fw_state("FAILED");
        return;
    }

    esp_err_t err = esp_ota_begin(s_update_partition, s_fw_size, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        report_fw_state("FAILED");
        return;
    }

    mbedtls_sha256_init(&s_sha_ctx);
    mbedtls_sha256_starts(&s_sha_ctx, 0 /* SHA-256, not SHA-224 */);

    /* Record what we're about to attempt, so that if this crashes/rolls
     * back, the next boot can recognize the failure and refuse to retry
     * automatically (see reconcile_pending_ota()). */
    save_nvs_str(NVS_KEY_PENDING_FW, s_fw_version);

    s_fw_update_in_progress = true;
    s_fw_received = 0;
    s_fw_chunk_index = 0;
    s_fw_request_id++;

    report_fw_state("DOWNLOADING");
    request_fw_chunk();
}

static void finish_fw_update(bool success)
{
    if (!success) {
        esp_ota_abort(s_ota_handle);
        report_fw_state("FAILED");
        s_fw_update_in_progress = false;
        return;
    }

    report_fw_state("DOWNLOADED");

    unsigned char digest[32];
    mbedtls_sha256_finish(&s_sha_ctx, digest);
    char digest_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(&digest_hex[i * 2], 3, "%02x", digest[i]);
    }

    bool checksum_ok = (strcasecmp(s_fw_checksum_algorithm, "SHA256") == 0) &&
                        (strcasecmp(digest_hex, s_fw_checksum) == 0);
    if (!checksum_ok) {
        ESP_LOGE(TAG, "Checksum mismatch or unsupported algorithm: got %s, expected %s (%s)",
                 digest_hex, s_fw_checksum, s_fw_checksum_algorithm);
        esp_ota_abort(s_ota_handle);
        report_fw_state("FAILED");
        s_fw_update_in_progress = false;
        return;
    }
    ESP_LOGI(TAG, "Checksum verified: %s", digest_hex);
    report_fw_state("VERIFIED");

    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        report_fw_state("FAILED");
        s_fw_update_in_progress = false;
        return;
    }

    report_fw_state("UPDATING");
    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        report_fw_state("FAILED");
        s_fw_update_in_progress = false;
        return;
    }

    char payload[256];
    snprintf(payload, sizeof(payload), "{\"current_fw_title\":\"%s\",\"current_fw_version\":\"%s\",\"fw_state\":\"UPDATED\"}",
             s_fw_title, s_fw_version);
    send_telemetry_json(payload);

    ESP_LOGW(TAG, "Update written and set as boot partition, restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void handle_fw_chunk_data(const char *data, int data_len)
{
    if (!s_fw_update_in_progress) {
        return;
    }

    esp_err_t err = esp_ota_write(s_ota_handle, data, data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        finish_fw_update(false);
        return;
    }
    mbedtls_sha256_update(&s_sha_ctx, (const unsigned char *)data, data_len);
    s_fw_received += data_len;

    ESP_LOGI(TAG, "Firmware chunk %d received: %d bytes (%d/%d total)",
             s_fw_chunk_index, data_len, s_fw_received, s_fw_size);

    if (data_len == 0 || s_fw_received >= s_fw_size) {
        finish_fw_update(s_fw_received == s_fw_size);
        return;
    }

    s_fw_chunk_index++;
    request_fw_chunk();
}

static void apply_shared_attribute(const char *key, cJSON *value)
{
    if (strcmp(key, "publishInterval") == 0 && cJSON_IsNumber(value)) {
        int new_interval = value->valueint;
        if (new_interval >= 1000) {
            s_publish_interval_ms = new_interval;
            ESP_LOGI(TAG, "publishInterval updated remotely to %d ms", s_publish_interval_ms);
        } else {
            ESP_LOGW(TAG, "Ignoring publishInterval=%d (below 1000 ms floor)", new_interval);
        }
    } else if (strcmp(key, "fw_title") == 0 && cJSON_IsString(value)) {
        strncpy(s_fw_title, value->valuestring, sizeof(s_fw_title) - 1);
    } else if (strcmp(key, "fw_version") == 0 && cJSON_IsString(value)) {
        strncpy(s_fw_version, value->valuestring, sizeof(s_fw_version) - 1);
    } else if (strcmp(key, "fw_checksum") == 0 && cJSON_IsString(value)) {
        strncpy(s_fw_checksum, value->valuestring, sizeof(s_fw_checksum) - 1);
    } else if (strcmp(key, "fw_checksum_algorithm") == 0 && cJSON_IsString(value)) {
        strncpy(s_fw_checksum_algorithm, value->valuestring, sizeof(s_fw_checksum_algorithm) - 1);
    } else if (strcmp(key, "fw_size") == 0 && cJSON_IsNumber(value)) {
        s_fw_size = value->valueint;
        s_fw_info_known = true;
    }
}

/* Two-way RPC: server sends {"method":..,"params":..} on
 * v1/devices/me/rpc/request/{requestId} and expects a reply, if any, on
 * v1/devices/me/rpc/response/{requestId} - unlike a Shared Attribute, this
 * is a synchronous request/response exchange, not a persisted value. */
static void handle_rpc_request(const char *topic, int topic_len, const char *data, int data_len)
{
    const char *request_id_str = topic + strlen(TB_RPC_REQUEST_TOPIC_PREFIX);
    int request_id_len = topic_len - (int)strlen(TB_RPC_REQUEST_TOPIC_PREFIX);
    char response_topic[64];
    snprintf(response_topic, sizeof(response_topic), "v1/devices/me/rpc/response/%.*s",
             request_id_len, request_id_str);

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse RPC request JSON");
        return;
    }
    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method)) {
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "RPC request: method=%s", method->valuestring);

    if (strcmp(method->valuestring, "getStatus") == 0) {
        char payload[128];
        snprintf(payload, sizeof(payload), "{\"temperature\":%d,\"freeHeap\":%" PRIu32 "}",
                 s_fake_temperature, esp_get_free_heap_size());
        esp_mqtt_client_publish(s_client, response_topic, payload, 0, 1, 0);
    } else if (strcmp(method->valuestring, "reboot") == 0) {
        esp_mqtt_client_publish(s_client, response_topic, "{\"result\":\"rebooting\"}", 0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGW(TAG, "Unknown RPC method: %s", method->valuestring);
        esp_mqtt_client_publish(s_client, response_topic, "{\"error\":\"unknown method\"}", 0, 1, 0);
    }

    cJSON_Delete(root);
}

static void handle_attributes_payload(const char *data, int data_len)
{
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse attributes JSON");
        return;
    }

    /* The /attributes/response/+ payload nests requested shared attributes
     * under a "shared" object; the plain /attributes push topic has the
     * keys at the top level. Handle both shapes. */
    cJSON *shared = cJSON_GetObjectItem(root, "shared");
    cJSON *container = shared ? shared : root;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, container) {
        apply_shared_attribute(item->string, item);
    }

    cJSON_Delete(root);

    if (s_fw_info_known && !s_fw_update_in_progress &&
        strlen(s_fw_version) > 0 && strcmp(s_fw_version, CURRENT_FW_VERSION) != 0) {
        start_fw_update();
    }
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void telemetry_task(void *arg)
{
    while (1) {
        if (s_mqtt_connected) {
            char payload[64];
            /* Real sensor reading would go here - a fake ramp is enough to
             * prove the pipeline (device -> broker -> dashboard) end to end. */
            s_fake_temperature = 20 + (s_fake_temperature + 1 - 20) % 10;
            snprintf(payload, sizeof(payload), "{\"temperature\":%d}", s_fake_temperature);

            int msg_id = esp_mqtt_client_publish(s_client, TB_TELEMETRY_TOPIC, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published telemetry: %s (msg_id=%d)", payload, msg_id);
        }
        vTaskDelay(pdMS_TO_TICKS(s_publish_interval_ms));
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED - device should show as Active on ThingsBoard now");
        s_mqtt_connected = true;
        esp_mqtt_client_publish(s_client, TB_TELEMETRY_TOPIC, "{\"active\":true}", 0, 1, 0);

        /* Subscribe to get pushed shared-attribute updates as they happen,
         * and separately ask for whatever is already set right now (a
         * fresh boot otherwise wouldn't see it until the next manual
         * change on the ThingsBoard console). Also report our current
         * firmware so it's visible on the ThingsBoard device view. */
        esp_mqtt_client_subscribe(s_client, TB_ATTRIBUTES_TOPIC, 1);
        esp_mqtt_client_subscribe(s_client, TB_ATTRIBUTES_RESPONSE_TOPIC, 1);
        esp_mqtt_client_subscribe(s_client, TB_FW_RESPONSE_TOPIC_FILTER, 1);
        esp_mqtt_client_subscribe(s_client, TB_RPC_REQUEST_TOPIC_FILTER, 1);
        esp_mqtt_client_publish(s_client, TB_ATTRIBUTES_REQUEST_TOPIC,
                                 "{\"sharedKeys\":\"publishInterval,fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm\"}",
                                 0, 1, 0);
        {
            char current_fw[128];
            snprintf(current_fw, sizeof(current_fw),
                     "{\"current_fw_title\":\"%s\",\"current_fw_version\":\"%s\"}",
                     CURRENT_FW_TITLE, CURRENT_FW_VERSION);
            send_telemetry_json(current_fw);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        s_mqtt_connected = false;
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA on topic=%.*s (%d bytes)", event->topic_len, event->topic, event->data_len);
        if (event->topic_len >= 6 && strncmp(event->topic, "v2/fw/", 6) == 0) {
            handle_fw_chunk_data(event->data, event->data_len);
        } else if (event->topic_len >= (int)strlen(TB_RPC_REQUEST_TOPIC_PREFIX) &&
                   strncmp(event->topic, TB_RPC_REQUEST_TOPIC_PREFIX, strlen(TB_RPC_REQUEST_TOPIC_PREFIX)) == 0) {
            handle_rpc_request(event->topic, event->topic_len, event->data, event->data_len);
        } else {
            handle_attributes_payload(event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static bool load_stored_token(char *out, size_t out_size)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_TOKEN, out, &len);
    nvs_close(handle);
    return err == ESP_OK;
}

static void save_token(const char *token)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, NVS_KEY_TOKEN, token));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}

static bool load_nvs_str(const char *key, char *out, size_t out_size)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    nvs_close(handle);
    return err == ESP_OK;
}

static void save_nvs_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, key, value));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}

static void erase_nvs_key(const char *key)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, key);
    nvs_commit(handle);
    nvs_close(handle);
}

/* Anti-loop protection: unlike ESP-IDF's native OTA API (which tracks a
 * "last invalid firmware version" itself), our hand-rolled ThingsBoard OTA
 * client has no built-in memory of a failed update - without this, a
 * still-assigned bad package on the server causes an infinite
 * download -> crash -> rollback -> download cycle (confirmed by testing).
 * Call once at boot, before anything else: reconciles whatever OTA attempt
 * was in flight when we last rebooted. */
static void reconcile_pending_ota(void)
{
    char pending[32];
    if (!load_nvs_str(NVS_KEY_PENDING_FW, pending, sizeof(pending))) {
        return;
    }
    if (strcmp(pending, CURRENT_FW_VERSION) == 0) {
        ESP_LOGI(TAG, "Successfully booted pending firmware %s - update confirmed", pending);
    } else {
        ESP_LOGW(TAG, "Previous OTA attempt to %s did not stick (running %s instead, likely rolled back) "
                       "- marking %s as known-bad, will not auto-retry it", pending, CURRENT_FW_VERSION, pending);
        save_nvs_str(NVS_KEY_FAILED_FW, pending);
    }
    erase_nvs_key(NVS_KEY_PENDING_FW);
}

/* MQTT device provisioning: on a factory-fresh board (no token saved in NVS
 * yet), connect "anonymously" (username "provision", no password) using a
 * key/secret shared by every device of this profile - not a per-device
 * secret - and ask ThingsBoard to create a device + issue us a real access
 * token, instead of a human copy-pasting one from the console per unit. */
static bool s_provisioning_done;
static char s_provisioned_token[64];

static void provisioning_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Provisioning: connected, requesting credentials");
        esp_mqtt_client_subscribe(client, TB_PROVISION_TOPIC_RESPONSE, 1);

        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char device_name[48];
        snprintf(device_name, sizeof(device_name), "esp32-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        char payload[256];
        snprintf(payload, sizeof(payload),
                 "{\"deviceName\":\"%s\",\"provisionDeviceKey\":\"%s\",\"provisionDeviceSecret\":\"%s\"}",
                 device_name, CONFIG_TB_PROVISION_KEY, CONFIG_TB_PROVISION_SECRET);
        esp_mqtt_client_publish(client, TB_PROVISION_TOPIC_REQUEST, payload, 0, 1, 0);
        break;
    }
    case MQTT_EVENT_DATA: {
        cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
        if (root != NULL) {
            cJSON *status = cJSON_GetObjectItem(root, "status");
            cJSON *credentials_value = cJSON_GetObjectItem(root, "credentialsValue");
            if (cJSON_IsString(status) && strcmp(status->valuestring, "SUCCESS") == 0 &&
                cJSON_IsString(credentials_value)) {
                strncpy(s_provisioned_token, credentials_value->valuestring, sizeof(s_provisioned_token) - 1);
                ESP_LOGI(TAG, "Provisioning succeeded, token issued");
            } else {
                ESP_LOGE(TAG, "Provisioning failed: %.*s", event->data_len, event->data);
            }
            cJSON_Delete(root);
        }
        s_provisioning_done = true;
        break;
    }
    default:
        break;
    }
}

static bool run_provisioning(char *out_token, size_t out_size)
{
    esp_mqtt_client_config_t prov_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = "provision",
    };
    esp_mqtt_client_handle_t prov_client = esp_mqtt_client_init(&prov_cfg);
    esp_mqtt_client_register_event(prov_client, ESP_EVENT_ANY_ID, provisioning_event_handler, NULL);
    esp_mqtt_client_start(prov_client);

    int waited_ms = 0;
    while (!s_provisioning_done && waited_ms < 15000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
    }

    esp_mqtt_client_stop(prov_client);
    esp_mqtt_client_destroy(prov_client);

    if (!s_provisioning_done || strlen(s_provisioned_token) == 0) {
        ESP_LOGE(TAG, "Provisioning timed out or failed");
        return false;
    }
    strncpy(out_token, s_provisioned_token, out_size - 1);
    return true;
}

static void mqtt_app_start(const char *access_token)
{
    /* ThingsBoard authenticates devices via MQTT username = access token
     * (no client certificate needed for this basic access-token auth
     * scheme - see notas_estagio.md for the alternative X.509 mutual-TLS
     * scheme AWS IoT Core uses instead). Transport itself is still
     * encrypted: mqtts:// (port 8883) with server certificate verified
     * against ESP-IDF's built-in trusted CA bundle, same one used for
     * regular HTTPS. Without this, the token above would travel in
     * plaintext over Wi-Fi - confirmed as a gap in earlier plaintext
     * mqtt:// testing (see notas_estagio.md). */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = access_token,
        /* Short keepalive so an abrupt disconnect (e.g. unplugging the
         * board) is detected quickly instead of waiting on the default
         * 120s. The Last Will is a message the broker sends on our behalf
         * the moment it notices we're gone - lets the dashboard reflect
         * "active: false" without waiting for the device to reconnect and
         * say so itself, which obviously can't happen if it's unplugged. */
        .session.keepalive = 15,
        .session.last_will.topic = TB_TELEMETRY_TOPIC,
        .session.last_will.msg = "{\"active\":false}",
        .session.last_will.qos = 1,
        /* Big enough to receive a whole firmware chunk (TB_FW_CHUNK_SIZE
         * bytes) in a single MQTT_EVENT_DATA callback, instead of dealing
         * with esp-mqtt's message fragmentation across several callbacks. */
        .buffer.size = TB_FW_CHUNK_SIZE + 1024,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("tb_mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    reconcile_pending_ota();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Same Wi-Fi helper used by the other PoCs (native_ota_example), configured
     * via menuconfig/sdkconfig.defaults - see examples/protocols/README.md */
    ESP_ERROR_CHECK(example_connect());

    /* Rollback confirmation, same pattern as esp-idf-ota/native_ota_example.c:
     * an image delivered via OTA boots in ESP_OTA_IMG_PENDING_VERIFY state.
     * Reaching this line means Wi-Fi came up fine, which is a reasonable
     * enough "diagnostic" for this PoC - confirm the image as good so the
     * bootloader stops treating it as provisional. If we never reach this
     * line (crash/hang/watchdog before it), the bootloader reverts to the
     * last known-good image on the next reset. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New OTA image passed basic diagnostics (Wi-Fi up) - marking valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }

    static char access_token[64];
    if (load_stored_token(access_token, sizeof(access_token))) {
        ESP_LOGI(TAG, "Using access token saved in NVS from a previous provisioning run");
    } else {
        ESP_LOGW(TAG, "No saved token - this looks like a factory-fresh board, provisioning now");
        if (!run_provisioning(access_token, sizeof(access_token))) {
            ESP_LOGE(TAG, "Provisioning failed, cannot continue");
            return;
        }
        save_token(access_token);
    }

    mqtt_app_start(access_token);

    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL);
}
