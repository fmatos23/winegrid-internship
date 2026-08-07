/* Origem: escrito de raiz nesta colaboração (Claude), não copiado de nenhum
 * repositório - implementa o protocolo MQTT/SAS-token do DPS diretamente
 * a partir da documentação oficial da Azure, sem usar o Azure SDK for
 * Embedded C.
 *
 * Azure IoT Hub Device Provisioning Service (DPS) PoC.

   Mirrors the ThingsBoard PoC's "factory-fresh board provisions itself"
   flow (esp-idf-thingsboard/main/app_main.c), but against Azure's
   equivalent: the device first talks to DPS (a fixed global endpoint,
   shared by every Azure customer) using a Symmetric Key Individual
   Enrollment, which hands back the specific IoT Hub + Device ID this
   registration was assigned to; only then does the device connect to that
   Hub to publish telemetry - two separate services/connections where
   ThingsBoard uses one.

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "protocol_examples_common.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"

static const char *TAG = "azure_dps_example";

#define DPS_GLOBAL_ENDPOINT "mqtts://global.azure-devices-provisioning.net:8883"
#define DPS_API_VERSION "2019-03-31"
#define IOTHUB_API_VERSION "2021-04-12"
#define SAS_TOKEN_VALIDITY_SECONDS 3600

#define NVS_NAMESPACE "azure_dps"
#define NVS_KEY_HUB "assigned_hub"
#define NVS_KEY_DEVICE_ID "device_id"
#define NVS_KEY_PENDING_FW "pending_fw"
#define NVS_KEY_FAILED_FW "failed_fw"

/* At the old 10s interval this was 8640 msgs/day - already over the Free
 * tier's 8000/day quota on its own, before Direct Method traffic. 60s
 * keeps it at ~1440 msgs/day (~18% of quota), with comfortable headroom -
 * also closer to a realistic sampling rate for a vineyard temperature
 * sensor than a 10s demo interval. Overridable at runtime via the device
 * twin's desired.publishInterval - see the Device Twin section below. */
#define TELEMETRY_PUBLISH_INTERVAL_MS_DEFAULT 60000
#define CURRENT_FW_TITLE "azure-dps-example"
#define CURRENT_FW_VERSION "1.0"
/* #define SIMULATE_BAD_OTA_IMAGE 1 -- uncomment + bump CURRENT_FW_VERSION to
 * rebuild the deliberately-crashing image used for the rollback/anti-loop
 * test (relatorio.tex, secção 7). */

/* ---- Time sync -----------------------------------------------------
 * SAS tokens embed an absolute Unix-epoch expiry ("se" field). Without a
 * real clock the ESP32 boots at 1970, so every token we generate would
 * already look expired to Azure - unlike the ThingsBoard PoC's static
 * access-token auth, this dependency on correct time is a hard
 * requirement here, not just a TLS-certificate nicety. */
static void sync_time(void)
{
    ESP_LOGI(TAG, "Syncing time via SNTP (required for SAS token expiry)...");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timed out - SAS tokens will likely be rejected as expired");
        return;
    }
    time_t now = time(NULL);
    ESP_LOGI(TAG, "Time synced: %s", ctime(&now));
}

/* ---- SAS token generation --------------------------------------------
 * Azure's device-facing auth (both DPS and IoT Hub) is a locally-computed
 * HMAC-SHA256 signature over "<url-encoded resource URI>\n<expiry>",
 * signed with the base64-decoded Symmetric Key, then re-encoded. The key
 * itself never goes over the wire - only this short-lived derived token
 * does. Equivalent role to the static access-token username used for
 * ThingsBoard, but computed fresh per connection instead of copy-pasted. */
static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 4 < dst_size; si++) {
        unsigned char c = (unsigned char)src[si];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = '\0';
}

/* ---- Group Enrollment key derivation -----------------------------------
 * Symmetric Key attestation for a *Group* Enrollment never provisions a
 * per-device secret in advance: every device of the model shares one group
 * key, and derives its own distinct key locally as
 * HMAC-SHA256(groupKey, registrationId) - the exact computation Azure also
 * performs server-side (with the same group key, kept in DPS) to verify the
 * SAS token this device then signs with that derived key. Individual
 * Enrollment (the default so far in this PoC) skips this: the configured
 * key already *is* this device's own key. */
static char s_effective_symmetric_key[128];

static void derive_group_device_key(const char *group_key_b64, const char *registration_id,
                                     char *out_key_b64, size_t out_size)
{
    unsigned char group_key[64];
    size_t group_key_len = 0;
    mbedtls_base64_decode(group_key, sizeof(group_key), &group_key_len,
                           (const unsigned char *)group_key_b64, strlen(group_key_b64));

    unsigned char derived[32];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md_info, group_key, group_key_len,
                     (const unsigned char *)registration_id, strlen(registration_id), derived);

    size_t len = 0;
    mbedtls_base64_encode((unsigned char *)out_key_b64, out_size, &len, derived, sizeof(derived));
    out_key_b64[len] = '\0';
}

static void init_effective_symmetric_key(void)
{
#if CONFIG_AZURE_DPS_USE_GROUP_ENROLLMENT
    derive_group_device_key(CONFIG_AZURE_DPS_SYMMETRIC_KEY, CONFIG_AZURE_DPS_REGISTRATION_ID,
                             s_effective_symmetric_key, sizeof(s_effective_symmetric_key));
    ESP_LOGI(TAG, "Group Enrollment mode: derived this device's key locally from the shared group key + registration ID '%s'",
             CONFIG_AZURE_DPS_REGISTRATION_ID);
#else
    strncpy(s_effective_symmetric_key, CONFIG_AZURE_DPS_SYMMETRIC_KEY, sizeof(s_effective_symmetric_key) - 1);
#endif
}

/* policy_name may be NULL (device-level IoT Hub auth has no named policy;
 * DPS individual enrollments authenticate against the fixed "registration"
 * policy). */
static bool build_sas_token(const char *resource_uri, const char *policy_name,
                             char *out_token, size_t out_size)
{
    unsigned char key[64];
    size_t key_len = 0;
    int rc = mbedtls_base64_decode(key, sizeof(key), &key_len,
                                    (const unsigned char *)s_effective_symmetric_key,
                                    strlen(s_effective_symmetric_key));
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to base64-decode the effective Symmetric Key (rc=%d) - check it was copied correctly", rc);
        return false;
    }

    long expiry = (long)time(NULL) + SAS_TOKEN_VALIDITY_SECONDS;

    char encoded_uri[192];
    url_encode(resource_uri, encoded_uri, sizeof(encoded_uri));

    char to_sign[256];
    snprintf(to_sign, sizeof(to_sign), "%s\n%ld", encoded_uri, expiry);

    unsigned char hmac_out[32];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md_info, key, key_len, (const unsigned char *)to_sign, strlen(to_sign), hmac_out);

    unsigned char sig_b64[64];
    size_t sig_b64_len = 0;
    mbedtls_base64_encode(sig_b64, sizeof(sig_b64), &sig_b64_len, hmac_out, sizeof(hmac_out));
    sig_b64[sig_b64_len] = '\0';

    char encoded_sig[96];
    url_encode((char *)sig_b64, encoded_sig, sizeof(encoded_sig));

    if (policy_name != NULL) {
        snprintf(out_token, out_size, "SharedAccessSignature sr=%s&sig=%s&se=%ld&skn=%s",
                 encoded_uri, encoded_sig, expiry, policy_name);
    } else {
        snprintf(out_token, out_size, "SharedAccessSignature sr=%s&sig=%s&se=%ld",
                 encoded_uri, encoded_sig, expiry);
    }
    return true;
}

/* ---- NVS helpers (assigned hub/device id cache) ---------------------- */

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

/* ---- DPS registration -------------------------------------------------
 * Same shape as run_provisioning() in the ThingsBoard PoC: connect, ask,
 * wait synchronously (with a timeout) for the event handler to flag
 * completion, then tear the client down. Unlike ThingsBoard's provisioning
 * (single request/response), DPS's "assigning" status requires re-polling
 * an operation id until Azure finishes allocating a hub - handled here via
 * s_dps_poll_pending instead of a single round trip. */
static bool s_dps_done;
static bool s_dps_success;
static int s_dps_request_id;
static char s_dps_operation_id[128];
static char s_assigned_hub[128];
static char s_device_id[128];
static volatile bool s_dps_poll_pending;

static void dps_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "DPS: connected, subscribing and requesting registration");
        esp_mqtt_client_subscribe(client, "$dps/registrations/res/#", 1);

        char topic[96];
        snprintf(topic, sizeof(topic), "$dps/registrations/PUT/iotdps-register/?$rid=%d", ++s_dps_request_id);
        char payload[96];
        snprintf(payload, sizeof(payload), "{\"registrationId\":\"%s\"}", CONFIG_AZURE_DPS_REGISTRATION_ID);
        esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
        break;
    }
    case MQTT_EVENT_DATA: {
        /* Topic shape: $dps/registrations/res/{status}/?$rid={id} - we only
         * need the status code to distinguish "still assigning" (202) from
         * a terminal response, the rest of the state comes from the JSON
         * body itself. */
        const char *prefix = "$dps/registrations/res/";
        char status_code[8] = {0};
        if (event->topic_len > (int)strlen(prefix) && strncmp(event->topic, prefix, strlen(prefix)) == 0) {
            const char *rest = event->topic + strlen(prefix);
            int max_len = event->topic_len - (int)strlen(prefix);
            int i = 0;
            while (i < max_len && rest[i] != '/' && i < (int)sizeof(status_code) - 1) {
                status_code[i] = rest[i];
                i++;
            }
        }

        cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
        if (root == NULL) {
            ESP_LOGW(TAG, "DPS: failed to parse response JSON");
            break;
        }

        cJSON *op_id = cJSON_GetObjectItem(root, "operationId");
        if (cJSON_IsString(op_id)) {
            strncpy(s_dps_operation_id, op_id->valuestring, sizeof(s_dps_operation_id) - 1);
        }

        cJSON *status = cJSON_GetObjectItem(root, "status");
        if (cJSON_IsString(status) && strcmp(status->valuestring, "assigned") == 0) {
            cJSON *reg_state = cJSON_GetObjectItem(root, "registrationState");
            cJSON *hub = cJSON_GetObjectItem(reg_state, "assignedHub");
            cJSON *dev_id = cJSON_GetObjectItem(reg_state, "deviceId");
            if (cJSON_IsString(hub) && cJSON_IsString(dev_id)) {
                strncpy(s_assigned_hub, hub->valuestring, sizeof(s_assigned_hub) - 1);
                strncpy(s_device_id, dev_id->valuestring, sizeof(s_device_id) - 1);
                s_dps_success = true;
                ESP_LOGI(TAG, "DPS: assigned to hub=%s deviceId=%s", s_assigned_hub, s_device_id);
            }
            s_dps_done = true;
        } else if (strcmp(status_code, "202") == 0) {
            ESP_LOGI(TAG, "DPS: still assigning (operationId=%s), will poll again", s_dps_operation_id);
            s_dps_poll_pending = true;
        } else {
            ESP_LOGE(TAG, "DPS: registration failed (status=%s): %.*s", status_code, event->data_len, event->data);
            s_dps_done = true;
        }
        cJSON_Delete(root);
        break;
    }
    default:
        break;
    }
}

static bool run_dps_registration(void)
{
    char resource_uri[128];
    snprintf(resource_uri, sizeof(resource_uri), "%s/registrations/%s",
             CONFIG_AZURE_DPS_ID_SCOPE, CONFIG_AZURE_DPS_REGISTRATION_ID);

    char sas_token[256];
    if (!build_sas_token(resource_uri, "registration", sas_token, sizeof(sas_token))) {
        return false;
    }

    char username[192];
    snprintf(username, sizeof(username), "%s/registrations/%s/api-version=%s",
             CONFIG_AZURE_DPS_ID_SCOPE, CONFIG_AZURE_DPS_REGISTRATION_ID, DPS_API_VERSION);

    esp_mqtt_client_config_t dps_cfg = {
        .broker.address.uri = DPS_GLOBAL_ENDPOINT,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = username,
        .credentials.client_id = CONFIG_AZURE_DPS_REGISTRATION_ID,
        .credentials.authentication.password = sas_token,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&dps_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, dps_mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    int waited_ms = 0;
    const int timeout_ms = 60000;
    while (!s_dps_done && waited_ms < timeout_ms) {
        if (s_dps_poll_pending) {
            s_dps_poll_pending = false;
            /* Azure's own SDKs default to a 2s retry-after when the server
             * doesn't specify one; keeping it fixed here instead of parsing
             * it out of the response is a deliberate scope trade-off for
             * this PoC. */
            vTaskDelay(pdMS_TO_TICKS(2000));
            char topic[256];
            snprintf(topic, sizeof(topic),
                     "$dps/registrations/GET/iotdps-get-operationstatus/?$rid=%d&operationId=%s",
                     ++s_dps_request_id, s_dps_operation_id);
            esp_mqtt_client_publish(client, topic, "", 0, 1, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
    }

    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);

    if (!s_dps_done || !s_dps_success) {
        ESP_LOGE(TAG, "DPS registration timed out or failed");
        return false;
    }
    return true;
}

/* ---- IoT Hub telemetry + Direct Methods --------------------------------
 * Telemetry, Direct Methods (Azure's synchronous request/response RPC) and
 * Device Twin (see below) - the equivalents already validated against
 * ThingsBoard's getStatus/reboot RPC and publishInterval shared attribute
 * (esp-idf-thingsboard/main/app_main.c). OTA is still out of scope for
 * this PoC. */
static esp_mqtt_client_handle_t s_hub_client;
static bool s_hub_connected;
static char s_hub_telemetry_topic[192];
static int s_fake_temperature = 20;

/* ---- Cloud-to-Device (C2D) messages ------------------------------------
 * Unlike Direct Methods (synchronous, device must respond within a
 * timeout) or Device Twin (persistent "last known state"), C2D is a
 * fire-and-forget queue: the cloud sends a message addressed to this
 * device, IoT Hub holds it (default TTL) until the device is connected and
 * subscribed, then delivers it - no response expected. Over MQTT, simply
 * receiving the PUBLISH (QoS 1) is enough to complete/dequeue it; no
 * separate accept/reject/abandon call like the AMQP protocol requires. */
static char s_c2d_topic_prefix[192];
static volatile bool s_c2d_force_telemetry;

#define DIRECT_METHOD_TOPIC_FILTER "$iothub/methods/POST/#"
#define DIRECT_METHOD_TOPIC_PREFIX "$iothub/methods/POST/"

/* Topic shape: $iothub/methods/POST/{method name}/?$rid={request id} -
 * unlike ThingsBoard's RPC (request id only, method name in the JSON
 * body), Azure puts the method name directly in the topic and the
 * request id as a query parameter, and expects the response published to
 * a mirrored $iothub/methods/res/{status}/?$rid={request id} topic. */
static void handle_direct_method_request(const char *topic, int topic_len, const char *data, int data_len)
{
    const char *after_prefix = topic + strlen(DIRECT_METHOD_TOPIC_PREFIX);
    int after_prefix_len = topic_len - (int)strlen(DIRECT_METHOD_TOPIC_PREFIX);

    char method_name[32] = {0};
    int name_len = 0;
    while (name_len < after_prefix_len && after_prefix[name_len] != '/' &&
           name_len < (int)sizeof(method_name) - 1) {
        method_name[name_len] = after_prefix[name_len];
        name_len++;
    }

    const char *rid_marker = "$rid=";
    const char *rid_pos = NULL;
    for (int i = name_len; i < after_prefix_len - (int)strlen(rid_marker); i++) {
        if (strncmp(after_prefix + i, rid_marker, strlen(rid_marker)) == 0) {
            rid_pos = after_prefix + i + strlen(rid_marker);
            break;
        }
    }
    if (rid_pos == NULL) {
        ESP_LOGW(TAG, "Direct method request missing $rid, ignoring");
        return;
    }
    int rid_len = (int)(after_prefix + after_prefix_len - rid_pos);

    char response_topic[128];
    snprintf(response_topic, sizeof(response_topic), "$iothub/methods/res/200/?$rid=%.*s", rid_len, rid_pos);

    ESP_LOGI(TAG, "Direct method invoked: %s", method_name);

    if (strcmp(method_name, "getStatus") == 0) {
        char payload[128];
        snprintf(payload, sizeof(payload), "{\"temperature\":%d,\"freeHeap\":%" PRIu32 "}",
                 s_fake_temperature, esp_get_free_heap_size());
        esp_mqtt_client_publish(s_hub_client, response_topic, payload, 0, 1, 0);
    } else if (strcmp(method_name, "reboot") == 0) {
        esp_mqtt_client_publish(s_hub_client, response_topic, "{\"result\":\"rebooting\"}", 0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        char not_found_topic[128];
        snprintf(not_found_topic, sizeof(not_found_topic), "$iothub/methods/res/404/?$rid=%.*s", rid_len, rid_pos);
        esp_mqtt_client_publish(s_hub_client, not_found_topic, "{\"error\":\"unknown method\"}", 0, 1, 0);
        ESP_LOGW(TAG, "Unknown direct method: %s", method_name);
    }
    (void)data;
    (void)data_len;
}

/* ---- Device Twin -------------------------------------------------------
 * Azure's equivalent to ThingsBoard's shared attributes (cloud->device,
 * here under properties.desired) plus a feature ThingsBoard's telemetry
 * doesn't have natively: properties.reported, a "last known good value"
 * per key that the device itself owns, instead of the cloud having to
 * infer current state from a telemetry stream. */
#define TWIN_RES_TOPIC_FILTER "$iothub/twin/res/#"
#define TWIN_RES_TOPIC_PREFIX "$iothub/twin/res/"
#define TWIN_DESIRED_TOPIC_FILTER "$iothub/twin/PATCH/properties/desired/#"
#define TWIN_DESIRED_TOPIC_PREFIX "$iothub/twin/PATCH/properties/desired/"
#define TWIN_GET_TOPIC_PREFIX "$iothub/twin/GET/?$rid="
#define TWIN_REPORTED_TOPIC_PREFIX "$iothub/twin/PATCH/properties/reported/?$rid="
#define MIN_PUBLISH_INTERVAL_MS 1000

static int s_publish_interval_ms = TELEMETRY_PUBLISH_INTERVAL_MS_DEFAULT;
static int s_twin_request_id;

/* Device-management state (firmware version, connectivity health) reported
 * to the twin - the criteria actually relevant to a device-management
 * platform (per orientador feedback): is this unit up to date, and is its
 * radio link healthy, not what its sensors are reading. */
static void report_twin_properties(void)
{
    wifi_ap_record_t ap_info;
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    char payload[224];
    snprintf(payload, sizeof(payload),
             "{\"current_fw_title\":\"%s\",\"current_fw_version\":\"%s\",\"publishInterval\":%d,"
             "\"wifiRssi\":%d,\"freeHeap\":%" PRIu32 "}",
             CURRENT_FW_TITLE, CURRENT_FW_VERSION, s_publish_interval_ms, rssi, esp_get_free_heap_size());
    char topic[64];
    snprintf(topic, sizeof(topic), "%s%d", TWIN_REPORTED_TOPIC_PREFIX, ++s_twin_request_id);
    esp_mqtt_client_publish(s_hub_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Reported twin properties: %s", payload);
}

/* Forward declaration - trigger_ota() is defined further below, after the
 * OTA block, but is needed here since apply_desired_properties() handles
 * every desired.* key in one place (interval and firmware alike). */
static void trigger_ota(const char *url, const char *new_version);

/* Same 1000ms sanity floor already used for the ThingsBoard shared
 * attribute (esp-idf-thingsboard/main/app_main.c) - a much lower value
 * would also risk blowing the 8000 msgs/day Free tier quota again. */
static void apply_desired_properties(cJSON *desired)
{
    cJSON *fw_version = cJSON_GetObjectItem(desired, "fw_version");
    cJSON *fw_url = cJSON_GetObjectItem(desired, "fw_url");
    if (cJSON_IsString(fw_version) && cJSON_IsString(fw_url) &&
        strcmp(fw_version->valuestring, CURRENT_FW_VERSION) != 0) {
        trigger_ota(fw_url->valuestring, fw_version->valuestring);
    }

    cJSON *interval = cJSON_GetObjectItem(desired, "publishInterval");
    if (!cJSON_IsNumber(interval)) {
        return;
    }
    int new_interval = interval->valueint;
    if (new_interval < MIN_PUBLISH_INTERVAL_MS) {
        ESP_LOGW(TAG, "Ignoring desired publishInterval=%d (below %dms floor)", new_interval, MIN_PUBLISH_INTERVAL_MS);
        return;
    }
    s_publish_interval_ms = new_interval;
    ESP_LOGI(TAG, "publishInterval updated remotely (device twin) to %d ms", s_publish_interval_ms);
    report_twin_properties();
}

/* ---- OTA, signaled via Device Twin --------------------------------------
 * Azure's official managed OTA service (Device Update for IoT Hub) isn't
 * available on the Free tier Hub used here, so the update itself is
 * signaled through the same Device Twin mechanism as publishInterval
 * (desired.fw_version + desired.fw_url), with the binary served over
 * plain HTTP by a local test server - same transport already validated in
 * esp-idf-ota/main/native_ota_example.c, just triggered by Azure instead
 * of a fixed compile-time URL. Progress is reported back via
 * reported.fw_state, mirroring the DOWNLOADING/VERIFIED/UPDATED state
 * machine already used for the ThingsBoard OTA client. */
static bool s_ota_in_progress;

static void report_fw_state(const char *state)
{
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"fw_state\":\"%s\"}", state);
    char topic[64];
    snprintf(topic, sizeof(topic), "%s%d", TWIN_REPORTED_TOPIC_PREFIX, ++s_twin_request_id);
    esp_mqtt_client_publish(s_hub_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "fw_state -> %s", state);
}

static void ota_task(void *arg)
{
    char *url = (char *)arg;
    char new_version[32];
    /* new_version was appended after the url, separated by a '\n', to
     * avoid a second heap allocation for a two-field task argument. */
    char *sep = strchr(url, '\n');
    strncpy(new_version, sep + 1, sizeof(new_version) - 1);
    new_version[sizeof(new_version) - 1] = '\0';
    *sep = '\0';

    ESP_LOGW(TAG, "Starting OTA: %s %s (current: %s %s)", CURRENT_FW_TITLE, new_version,
             CURRENT_FW_TITLE, CURRENT_FW_VERSION);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL || esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open OTA HTTP connection to %s", url);
        report_fw_state("FAILED");
        goto done;
    }
    esp_http_client_fetch_headers(client);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t update_handle;
    if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed");
        esp_http_client_cleanup(client);
        report_fw_state("FAILED");
        goto done;
    }

    save_nvs_str(NVS_KEY_PENDING_FW, new_version);
    report_fw_state("DOWNLOADING");

    static char buf[1024];
    int total = 0;
    bool download_ok = true;
    while (1) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            download_ok = false;
            break;
        } else if (n > 0) {
            if (esp_ota_write(update_handle, buf, n) != ESP_OK) {
                download_ok = false;
                break;
            }
            total += n;
        } else if (esp_http_client_is_complete_data_received(client)) {
            break;
        } else {
            download_ok = false;
            break;
        }
    }
    esp_http_client_cleanup(client);

    if (!download_ok) {
        ESP_LOGE(TAG, "OTA download failed after %d bytes", total);
        esp_ota_abort(update_handle);
        report_fw_state("FAILED");
        erase_nvs_key(NVS_KEY_PENDING_FW);
        goto done;
    }
    ESP_LOGI(TAG, "OTA download complete: %d bytes", total);
    report_fw_state("VERIFIED");

    if (esp_ota_end(update_handle) != ESP_OK || esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end/set_boot_partition failed");
        report_fw_state("FAILED");
        erase_nvs_key(NVS_KEY_PENDING_FW);
        goto done;
    }

    report_fw_state("UPDATED");
    ESP_LOGW(TAG, "Update written, restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

done:
    free(url);
    s_ota_in_progress = false;
    vTaskDelete(NULL);
}

/* Anti-loop protection, same reasoning and NVS-backed mechanism as
 * reconcile_pending_ota() in the ThingsBoard PoC: without it, a
 * still-desired bad version in the twin causes an infinite
 * download->crash->rollback->download cycle. */
static void trigger_ota(const char *url, const char *new_version)
{
    if (s_ota_in_progress) {
        return;
    }
    char failed_version[32];
    if (load_nvs_str(NVS_KEY_FAILED_FW, failed_version, sizeof(failed_version)) &&
        strcmp(failed_version, new_version) == 0) {
        ESP_LOGE(TAG, "Firmware %s previously failed on this device - refusing to retry automatically "
                       "(set a different desired.fw_version to retry)", new_version);
        report_fw_state("FAILED");
        return;
    }

    s_ota_in_progress = true;
    /* url + '\n' + new_version packed into one allocation - see ota_task(). */
    char *arg = malloc(strlen(url) + 1 + strlen(new_version) + 1);
    sprintf(arg, "%s\n%s", url, new_version);
    xTaskCreate(ota_task, "ota_task", 8192, arg, 5, NULL);
}

static void hub_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "IoT Hub: connected, device should show as connected in the portal now");
        s_hub_connected = true;
        esp_mqtt_client_subscribe(event->client, DIRECT_METHOD_TOPIC_FILTER, 1);
        esp_mqtt_client_subscribe(event->client, TWIN_RES_TOPIC_FILTER, 1);
        esp_mqtt_client_subscribe(event->client, TWIN_DESIRED_TOPIC_FILTER, 1);
        {
            char c2d_filter[200];
            snprintf(c2d_filter, sizeof(c2d_filter), "%s#", s_c2d_topic_prefix);
            esp_mqtt_client_subscribe(event->client, c2d_filter, 1);
        }

        /* Ask for whatever desired properties are already set (a fresh
         * connection otherwise wouldn't see them until the next manual
         * change on the console - same reasoning as the ThingsBoard
         * attributes/request call), and report our current state. */
        char topic[48];
        snprintf(topic, sizeof(topic), "%s%d", TWIN_GET_TOPIC_PREFIX, ++s_twin_request_id);
        esp_mqtt_client_publish(event->client, topic, "", 0, 1, 0);
        report_twin_properties();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "IoT Hub: disconnected");
        s_hub_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len >= (int)strlen(DIRECT_METHOD_TOPIC_PREFIX) &&
            strncmp(event->topic, DIRECT_METHOD_TOPIC_PREFIX, strlen(DIRECT_METHOD_TOPIC_PREFIX)) == 0) {
            handle_direct_method_request(event->topic, event->topic_len, event->data, event->data_len);
        } else if (event->topic_len >= (int)strlen(TWIN_DESIRED_TOPIC_PREFIX) &&
                   strncmp(event->topic, TWIN_DESIRED_TOPIC_PREFIX, strlen(TWIN_DESIRED_TOPIC_PREFIX)) == 0) {
            /* Push notification of a change - payload is the raw patch,
             * desired properties directly at the root (no "desired"
             * wrapper, unlike the full GET response below). */
            cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
            if (root != NULL) {
                apply_desired_properties(root);
                cJSON_Delete(root);
            }
        } else if (event->topic_len >= (int)strlen(TWIN_RES_TOPIC_PREFIX) &&
                   strncmp(event->topic, TWIN_RES_TOPIC_PREFIX, strlen(TWIN_RES_TOPIC_PREFIX)) == 0) {
            /* Response to our GET request - full twin, desired properties
             * nested under a "desired" key alongside "reported". */
            cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
            if (root != NULL) {
                cJSON *desired = cJSON_GetObjectItem(root, "desired");
                if (desired != NULL) {
                    apply_desired_properties(desired);
                }
                cJSON_Delete(root);
            }
        } else if (event->topic_len >= (int)strlen(s_c2d_topic_prefix) &&
                   strncmp(event->topic, s_c2d_topic_prefix, strlen(s_c2d_topic_prefix)) == 0) {
            /* No explicit accept/reject needed over MQTT - just receiving
             * this PUBLISH already dequeues it on the Hub side. As a
             * concrete demo reaction (rather than only logging), treat any
             * C2D message as "send a telemetry reading right now" instead
             * of waiting for the next scheduled interval - e.g. useful for
             * an on-demand reading from a vineyard sensor between its
             * normal 60s/90s cycle. */
            ESP_LOGW(TAG, "C2D message received: %.*s", event->data_len, event->data);
            s_c2d_force_telemetry = true;
        }
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "IoT Hub: transport error, esp-tls last error 0x%x",
                     event->error_handle->esp_tls_last_esp_err);
        }
        break;
    default:
        break;
    }
}

static void telemetry_task(void *arg)
{
    int waited_ms = 0;
    while (1) {
        bool forced = s_c2d_force_telemetry;
        if (s_hub_connected && (waited_ms >= s_publish_interval_ms || forced)) {
            if (forced) {
                s_c2d_force_telemetry = false;
                ESP_LOGI(TAG, "Publishing telemetry immediately (C2D request), resetting the regular cycle");
            }
            char payload[64];
            s_fake_temperature = 20 + (s_fake_temperature + 1 - 20) % 10;
            snprintf(payload, sizeof(payload), "{\"temperature\":%d}", s_fake_temperature);
            int msg_id = esp_mqtt_client_publish(s_hub_client, s_hub_telemetry_topic, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published telemetry: %s (msg_id=%d)", payload, msg_id);
            waited_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
    }
}

static void connect_to_hub(const char *hub_hostname, const char *device_id)
{
    char resource_uri[160];
    snprintf(resource_uri, sizeof(resource_uri), "%s/devices/%s", hub_hostname, device_id);

    char sas_token[256];
    if (!build_sas_token(resource_uri, NULL, sas_token, sizeof(sas_token))) {
        ESP_LOGE(TAG, "Failed to build IoT Hub SAS token");
        return;
    }

    char username[192];
    snprintf(username, sizeof(username), "%s/%s/?api-version=%s", hub_hostname, device_id, IOTHUB_API_VERSION);

    /* $.ct/$.ce declare the body as JSON/UTF-8 - without these, IoT Hub
     * Message Routing can't evaluate a query against $body.* (it would
     * only be able to match on application/system properties, not body
     * content). */
    snprintf(s_hub_telemetry_topic, sizeof(s_hub_telemetry_topic),
             "devices/%s/messages/events/$.ct=application%%2Fjson&$.ce=utf-8", device_id);
    snprintf(s_c2d_topic_prefix, sizeof(s_c2d_topic_prefix), "devices/%s/messages/devicebound/", device_id);

    char broker_uri[192];
    snprintf(broker_uri, sizeof(broker_uri), "mqtts://%s:8883", hub_hostname);

    /* SAS token above is only valid for SAS_TOKEN_VALIDITY_SECONDS - fine
     * for this PoC's short test runs, but a long-lived deployment would
     * need to regenerate and reconnect with a fresh token before expiry
     * (not implemented here, same scope trade-off as the polling interval
     * above). */
    esp_mqtt_client_config_t hub_cfg = {
        .broker.address.uri = broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = username,
        .credentials.client_id = device_id,
        .credentials.authentication.password = sas_token,
    };
    s_hub_client = esp_mqtt_client_init(&hub_cfg);
    esp_mqtt_client_register_event(s_hub_client, ESP_EVENT_ANY_ID, hub_mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_hub_client);

    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL);
}

/* Same reconciliation as the ThingsBoard PoC: called once at boot, before
 * anything else that could crash - resolves whatever OTA attempt was in
 * flight when the device last rebooted, marking a version that didn't
 * stick (rolled back) as known-bad so trigger_ota() refuses to retry it
 * automatically. */
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

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);

    init_effective_symmetric_key();

#if defined(SIMULATE_BAD_OTA_IMAGE)
    /* Deliberate crash for the rollback/anti-loop test - same pattern as
     * the abort() test already documented for ThingsBoard/ESP-IDF nativo
     * (relatorio.tex). MUST run before reconcile_pending_ota(): that call
     * erases NVS_KEY_PENDING_FW as soon as CURRENT_FW_VERSION matches it,
     * regardless of whether this boot is about to crash - discovered via a
     * real failed test run here, where the pending marker got cleared on
     * the bad boot itself (before the crash), so the anti-loop check on
     * the *next* boot (the one the bootloader rolls back to) found nothing
     * to compare against and never flagged the version as known-bad,
     * causing an infinite download->crash->rollback->download loop. */
    ESP_LOGE(TAG, "SIMULATE_BAD_OTA_IMAGE build - crashing on purpose");
    abort();
#endif

    ESP_ERROR_CHECK(nvs_flash_init());
    reconcile_pending_ota();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Same Wi-Fi helper used by the other PoCs, configured via
     * menuconfig/sdkconfig.defaults - see examples/protocols/README.md */
    ESP_ERROR_CHECK(example_connect());

    /* Rollback confirmation, same pattern as esp-idf-ota/esp-idf-thingsboard:
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

    sync_time();

    static char hub_hostname[128];
    static char device_id[128];

    if (load_nvs_str(NVS_KEY_HUB, hub_hostname, sizeof(hub_hostname)) &&
        load_nvs_str(NVS_KEY_DEVICE_ID, device_id, sizeof(device_id))) {
        ESP_LOGI(TAG, "Using hub assignment saved in NVS from a previous DPS run: hub=%s deviceId=%s",
                 hub_hostname, device_id);
    } else {
        ESP_LOGW(TAG, "No saved hub assignment - this looks like a factory-fresh board, registering with DPS now");
        if (!run_dps_registration()) {
            ESP_LOGE(TAG, "DPS registration failed, cannot continue");
            return;
        }
        strncpy(hub_hostname, s_assigned_hub, sizeof(hub_hostname) - 1);
        strncpy(device_id, s_device_id, sizeof(device_id) - 1);
        save_nvs_str(NVS_KEY_HUB, hub_hostname);
        save_nvs_str(NVS_KEY_DEVICE_ID, device_id);
    }

    connect_to_hub(hub_hostname, device_id);
}