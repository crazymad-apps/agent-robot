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
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "host_discovery.h"
#include "network_connectivity.h"
#include "nvs_flash.h"
#include "wss_transport.h"

#define NETWORK_READY_BIT BIT0

static const char *const TAG = "transport_validation";
static EventGroupHandle_t network_events;

static void validation_fail(const char *step, int result)
{
    ESP_LOGE(TAG, "step=%s result=%d status=failed", step, result);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void network_event(
    const network_connectivity_event_t *event,
    void *context)
{
    EventGroupHandle_t events = context;
    if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
        xEventGroupSetBits(events, NETWORK_READY_BIT);
    } else if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_LOST) {
        xEventGroupClearBits(events, NETWORK_READY_BIT);
    }
}

static network_connectivity_result_t network_initialize(
    network_connectivity_t **connectivity)
{
    network_events = xEventGroupCreate();
    if (network_events == NULL) {
        return NETWORK_CONNECTIVITY_RESULT_NO_MEMORY;
    }
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) {
        vEventGroupDelete(network_events);
        network_events = NULL;
        return NETWORK_CONNECTIVITY_RESULT_PLATFORM_ERROR;
    }
    const network_connectivity_config_t config = {
        .event_callback = network_event,
        .event_context = network_events,
    };
    network_connectivity_result_t result =
        network_connectivity_create(&config, connectivity);
    if (result == NETWORK_CONNECTIVITY_RESULT_OK) {
        result = network_connectivity_start(*connectivity);
    }
    if (result != NETWORK_CONNECTIVITY_RESULT_OK && *connectivity != NULL) {
        network_connectivity_destroy(*connectivity);
        *connectivity = NULL;
    }
    if (result != NETWORK_CONNECTIVITY_RESULT_OK) {
        vEventGroupDelete(network_events);
        network_events = NULL;
    }
    return result;
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
    network_connectivity_t *connectivity = NULL;
    const network_connectivity_result_t network_result =
        network_initialize(&connectivity);
    if (network_result != NETWORK_CONNECTIVITY_RESULT_OK) {
        validation_fail("network_initialize", network_result);
    }
    const EventBits_t connected = xEventGroupWaitBits(
        network_events, NETWORK_READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000U));
    if ((connected & NETWORK_READY_BIT) == 0U) {
        validation_fail("network_ready", ESP_ERR_TIMEOUT);
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
    host_discovery_deinitialize();
    network_connectivity_destroy(connectivity);
    vEventGroupDelete(network_events);
    ESP_LOGI(TAG, "target discovery, TLS pin, and WSS validation passed");
}
