#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_control_envelope.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "host_discovery.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wss_transport.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"

static const char *const TAG = "transport_validation";
static EventGroupHandle_t wifi_events;

static void validation_fail(const char *step, int result)
{
    ESP_LOGE(TAG, "step=%s result=%d status=failed", step, result);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static esp_err_t load_wifi_credentials(
    char *ssid,
    size_t ssid_capacity,
    char *password,
    size_t password_capacity)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return result;
    }
    size_t ssid_length = ssid_capacity;
    result = nvs_get_str(handle, WIFI_NVS_SSID_KEY, ssid, &ssid_length);
    if (result == ESP_OK) {
        size_t password_length = password_capacity;
        result = nvs_get_str(handle, WIFI_NVS_PASSWORD_KEY, password, &password_length);
    }
    nvs_close(handle);
    return result;
}

static void wifi_event(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_data;
    if (event_id == WIFI_EVENT_STA_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        (void)esp_wifi_connect();
    }
}

static void ip_event(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_data;
    if (event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_initialize(void)
{
    char ssid[33U] = {0};
    char password[64U] = {0};
    esp_err_t result = load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (result != ESP_OK) {
        return result;
    }
    wifi_events = xEventGroupCreate();
    if (wifi_events == NULL) {
        memset(password, 0, sizeof(password));
        return ESP_ERR_NO_MEM;
    }
    if ((result = esp_netif_init()) != ESP_OK ||
        (result = esp_event_loop_create_default()) != ESP_OK ||
        esp_netif_create_default_wifi_sta() == NULL) {
        memset(password, 0, sizeof(password));
        return result == ESP_OK ? ESP_ERR_NO_MEM : result;
    }
    const wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    if ((result = esp_wifi_init(&initialization)) != ESP_OK ||
        (result = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (result = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (result = esp_event_handler_register(
             WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)) != ESP_OK ||
        (result = esp_event_handler_register(
             IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL)) != ESP_OK) {
        memset(password, 0, sizeof(password));
        return result;
    }

    wifi_config_t configuration = {0};
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    memcpy(configuration.sta.ssid, ssid, ssid_length);
    memcpy(configuration.sta.password, password, password_length);
    configuration.sta.threshold.authmode =
        password_length == 0U ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;
    result = esp_wifi_set_config(WIFI_IF_STA, &configuration);
    memset(password, 0, sizeof(password));
    if (result != ESP_OK) {
        return result;
    }
    esp_log_level_set("wifi", ESP_LOG_WARN);
    return esp_wifi_start();
}

static bool copy_endpoint(
    const host_discovery_record_t *record,
    wss_transport_endpoint_t *endpoint)
{
    if (strlen(record->path) >= sizeof(endpoint->path) ||
        strlen(record->certificate_fingerprint) >= sizeof(endpoint->certificate_fingerprint)) {
        return false;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->ipv4_address = record->ipv4_address;
    endpoint->port = record->port;
    strcpy(endpoint->path, record->path);
    strcpy(endpoint->certificate_fingerprint, record->certificate_fingerprint);
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting target discovery, TLS pin, and WSS validation");
    const esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        validation_fail("nvs_initialize", nvs_result);
    }
    const esp_err_t wifi_result = wifi_initialize();
    if (wifi_result != ESP_OK) {
        validation_fail("wifi_initialize", wifi_result);
    }
    const EventBits_t connected = xEventGroupWaitBits(
        wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000U));
    if ((connected & WIFI_CONNECTED_BIT) == 0U) {
        validation_fail("wifi_connect", ESP_ERR_TIMEOUT);
    }

    host_discovery_result_t discovery_result = host_discovery_initialize();
    if (discovery_result != HOST_DISCOVERY_RESULT_OK) {
        validation_fail("discovery_initialize", discovery_result);
    }
    host_discovery_record_t records[HOST_DISCOVERY_MAX_RESULTS];
    size_t record_count = 0U;
    discovery_result = host_discovery_query(
        5000U, records, HOST_DISCOVERY_MAX_RESULTS, &record_count);
    if (discovery_result != HOST_DISCOVERY_RESULT_OK || record_count == 0U) {
        validation_fail("discovery_query", discovery_result);
    }

    const host_discovery_record_t *candidate = NULL;
    for (size_t index = 0U; index < record_count; ++index) {
        if (host_discovery_record_is_compatible(&records[index])) {
            candidate = &records[index];
            break;
        }
    }
    if (candidate == NULL) {
        validation_fail("compatible_host", HOST_SELECTION_NOT_FOUND);
    }
    host_selection_identity_t identity = {.paired = true};
    strcpy(identity.installation_id, candidate->installation_id);
    strcpy(identity.certificate_fingerprint, candidate->certificate_fingerprint);
    host_discovery_record_t selected;
    const host_selection_result_t selection_result = host_discovery_select(
        records, record_count, &identity, &selected);
    if (selection_result != HOST_SELECTION_SELECTED) {
        validation_fail("host_selection", selection_result);
    }

    wss_transport_endpoint_t endpoint;
    if (!copy_endpoint(&selected, &endpoint)) {
        validation_fail("endpoint_copy", WSS_TRANSPORT_RESULT_INVALID_ARGUMENT);
    }
    wss_transport_t *transport = NULL;
    wss_transport_result_t transport_result = wss_transport_create(&transport);
    if (transport_result != WSS_TRANSPORT_RESULT_OK) {
        validation_fail("transport_create", transport_result);
    }

    wss_transport_endpoint_t rejected_endpoint = endpoint;
    rejected_endpoint.certificate_fingerprint[0] =
        rejected_endpoint.certificate_fingerprint[0] == '0' ? '1' : '0';
    transport_result = wss_transport_connect(
        transport, &rejected_endpoint, 5000U, 10000U, 10000U);
    if (transport_result != WSS_TRANSPORT_RESULT_CERTIFICATE_MISMATCH) {
        validation_fail("certificate_mismatch", transport_result);
    }
    ESP_LOGI(TAG, "certificate mismatch blocked before WebSocket Upgrade");

    transport_result =
        wss_transport_connect(transport, &endpoint, 5000U, 10000U, 10000U);
    if (transport_result != WSS_TRANSPORT_RESULT_OK) {
        validation_fail("wss_connect", transport_result);
    }
    uint8_t *payload = heap_caps_malloc(
        WSS_TRANSPORT_CONTROL_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL) {
        validation_fail("receive_buffer", WSS_TRANSPORT_RESULT_NO_MEMORY);
    }
    wss_transport_message_t message;
    transport_result = wss_transport_receive(
        transport, payload, WSS_TRANSPORT_CONTROL_MAX_BYTES, &message, 10000U);
    if (transport_result != WSS_TRANSPORT_RESULT_OK ||
        message.type != WSS_TRANSPORT_MESSAGE_TEXT) {
        validation_fail("auth_challenge_receive", transport_result);
    }
    device_control_envelope_t *envelope = NULL;
    const device_protocol_result_t protocol_result = device_control_envelope_decode(
        (const char *)payload, message.payload_length, &envelope);
    if (protocol_result != DEVICE_PROTOCOL_RESULT_OK || envelope == NULL ||
        strcmp(device_control_envelope_message_type(envelope), "auth_challenge") != 0) {
        validation_fail("auth_challenge_decode", protocol_result);
    }
    device_control_envelope_release(envelope);
    (void)wss_transport_send_close(transport, 1000U, 1000U);
    wss_transport_destroy(transport);
    free(payload);
    ESP_LOGI(TAG, "target discovery, TLS pin, and WSS validation passed");
}
