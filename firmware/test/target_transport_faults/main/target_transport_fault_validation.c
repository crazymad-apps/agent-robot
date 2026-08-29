#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "device_connection_supervisor.h"
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
#define VALIDATION_RECONNECT_DELAY_MS 250U

static const char *const TAG = "transport_fault_validation";
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

static void connect_expect(
    wss_transport_t *transport,
    const wss_transport_endpoint_t *endpoint,
    wss_transport_result_t expected,
    const char *step)
{
    const wss_transport_result_t result =
        wss_transport_connect(transport, endpoint, 5000U, 10000U, 10000U);
    if (result != expected) {
        validation_fail(step, result);
    }
}

static void reconnect_delay(void)
{
    vTaskDelay(pdMS_TO_TICKS(VALIDATION_RECONNECT_DELAY_MS));
}

static void verify_supervisor_boundaries(void)
{
    device_connection_supervisor_t supervisor;
    device_connection_supervisor_initialize(&supervisor);
    device_connection_supervisor_got_ip(&supervisor, 0U);
    uint64_t epoch = 0U;
    if (!device_connection_supervisor_connect_discovered(&supervisor, &epoch) ||
        !device_connection_supervisor_transport_ready(&supervisor, epoch) ||
        !device_connection_supervisor_authenticated(&supervisor, epoch, 1000U) ||
        device_connection_supervisor_poll(&supervisor, 30999U, 200U, &epoch) !=
            DEVICE_CONNECTION_ACTION_NONE ||
        device_connection_supervisor_poll(&supervisor, 31000U, 200U, &epoch) !=
            DEVICE_CONNECTION_ACTION_NONE ||
        supervisor.state != DEVICE_CONNECTION_STATE_BACKOFF ||
        supervisor.backoff_until_ms != 32000U || supervisor.consecutive_failures != 1U) {
        validation_fail("heartbeat_timeout", -1);
    }
    const uint64_t stale_epoch = epoch;
    device_connection_supervisor_wifi_down(&supervisor, 31001U);
    device_connection_supervisor_got_ip(&supervisor, 31002U);
    if (device_connection_supervisor_receive_activity(&supervisor, stale_epoch, 31003U)) {
        validation_fail("stale_epoch", -1);
    }
}

static void verify_fragmented_exchange(
    wss_transport_t *transport,
    uint8_t *payload,
    size_t payload_capacity)
{
    wss_transport_message_t message;
    wss_transport_result_t result =
        wss_transport_receive(transport, payload, payload_capacity, &message, 5000U);
    if (result != WSS_TRANSPORT_RESULT_OK || message.type != WSS_TRANSPORT_MESSAGE_PING ||
        message.payload_length != 5U || memcmp(payload, "probe", 5U) != 0) {
        validation_fail("interleaved_ping", result);
    }

    result = wss_transport_receive(transport, payload, payload_capacity, &message, 5000U);
    if (result != WSS_TRANSPORT_RESULT_OK || message.type != WSS_TRANSPORT_MESSAGE_TEXT ||
        message.payload_length != WSS_TRANSPORT_CONTROL_MAX_BYTES) {
        validation_fail("fragmented_text", result);
    }
    for (size_t index = 0U; index < message.payload_length; ++index) {
        const uint8_t expected = index < message.payload_length / 2U ? 0x41U : 0x42U;
        if (payload[index] != expected) {
            validation_fail("fragmented_text_payload", (int)index);
        }
    }

    result = wss_transport_receive(transport, payload, payload_capacity, &message, 5000U);
    if (result != WSS_TRANSPORT_RESULT_OK || message.type != WSS_TRANSPORT_MESSAGE_BINARY ||
        message.payload_length != 656U) {
        validation_fail("binary_frame", result);
    }
    for (size_t index = 0U; index < message.payload_length; ++index) {
        if (payload[index] != index % 251U) {
            validation_fail("binary_payload", (int)index);
        }
    }

    result = wss_transport_receive(transport, payload, payload_capacity, &message, 5000U);
    if (result != WSS_TRANSPORT_RESULT_OK || message.type != WSS_TRANSPORT_MESSAGE_CLOSE ||
        message.close_code != 1001U || wss_transport_is_connected(transport)) {
        validation_fail("close_echo", result);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting target transport fault validation");
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
    if (discovery_result != HOST_DISCOVERY_RESULT_OK) {
        validation_fail("discovery_query", discovery_result);
    }
    const host_selection_identity_t identity = {.paired = false};
    host_discovery_record_t selected;
    const host_selection_result_t selection_result =
        host_discovery_select(records, record_count, &identity, &selected);
    if (selection_result != HOST_SELECTION_SELECTED) {
        validation_fail("host_selection", selection_result);
    }

    wss_transport_endpoint_t endpoint;
    if (!copy_endpoint(&selected, &endpoint)) {
        validation_fail("endpoint_copy", WSS_TRANSPORT_RESULT_INVALID_ARGUMENT);
    }
    wss_transport_t *transport = NULL;
    wss_transport_result_t result = wss_transport_create(&transport);
    if (result != WSS_TRANSPORT_RESULT_OK) {
        validation_fail("transport_create", result);
    }

    wss_transport_endpoint_t rejected_endpoint = endpoint;
    rejected_endpoint.certificate_fingerprint[0] =
        rejected_endpoint.certificate_fingerprint[0] == '0' ? '1' : '0';
    connect_expect(
        transport,
        &rejected_endpoint,
        WSS_TRANSPORT_RESULT_CERTIFICATE_MISMATCH,
        "certificate_rejection");
    reconnect_delay();

    connect_expect(
        transport, &endpoint, WSS_TRANSPORT_RESULT_UPGRADE_REJECTED, "upgrade_rejection");
    reconnect_delay();

    uint8_t *payload = heap_caps_malloc(
        WSS_TRANSPORT_CONTROL_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL) {
        validation_fail("receive_buffer", WSS_TRANSPORT_RESULT_NO_MEMORY);
    }
    connect_expect(transport, &endpoint, WSS_TRANSPORT_RESULT_OK, "fragment_connect");
    verify_fragmented_exchange(
        transport, payload, WSS_TRANSPORT_CONTROL_MAX_BYTES);
    wss_transport_disconnect(transport);
    reconnect_delay();

    connect_expect(transport, &endpoint, WSS_TRANSPORT_RESULT_OK, "oversized_connect");
    wss_transport_message_t message;
    result = wss_transport_receive(
        transport, payload, WSS_TRANSPORT_CONTROL_MAX_BYTES, &message, 5000U);
    if (result != WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED) {
        validation_fail("oversized_text", result);
    }
    reconnect_delay();

    connect_expect(transport, &endpoint, WSS_TRANSPORT_RESULT_OK, "partial_connect");
    const uint64_t receive_time_before_partial = wss_transport_last_receive_time_ms(transport);
    result = wss_transport_receive(
        transport, payload, WSS_TRANSPORT_CONTROL_MAX_BYTES, &message, 1000U);
    if (result != WSS_TRANSPORT_RESULT_TIMEOUT ||
        wss_transport_last_receive_time_ms(transport) != receive_time_before_partial) {
        validation_fail("partial_frame_timeout", result);
    }
    reconnect_delay();

    connect_expect(transport, &endpoint, WSS_TRANSPORT_RESULT_OK, "masked_frame_connect");
    result = wss_transport_receive(
        transport, payload, WSS_TRANSPORT_CONTROL_MAX_BYTES, &message, 5000U);
    if (result != WSS_TRANSPORT_RESULT_PROTOCOL_ERROR) {
        validation_fail("masked_server_frame", result);
    }

    verify_supervisor_boundaries();
    wss_transport_destroy(transport);
    free(payload);
    ESP_LOGI(TAG, "target transport fault validation passed");
}
