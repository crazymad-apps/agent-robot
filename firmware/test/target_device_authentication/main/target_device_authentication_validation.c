#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "device_connection.h"
#include "device_ed25519.h"
#include "device_identity.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host_discovery.h"
#include "network_connectivity.h"
#include "nvs_flash.h"

#define VALIDATION_HOST_INSTALLATION_ID "fa0177a11da74ff089ec0de200000003"
#define VALIDATION_DEVICE_ID "device-auth-validation"
#define VALIDATION_NETWORK_READY_BIT BIT0
#define VALIDATION_PENDING_CLEARED_BIT BIT1
#define VALIDATION_ONLINE_BIT BIT2
#define VALIDATION_REVOKED_BIT BIT3
#define VALIDATION_FAILED_BIT BIT4
#define VALIDATION_EVENT_QUEUE_LENGTH 24U

typedef enum {
    VALIDATION_STAGE_PENDING_REJECTION = 0,
    VALIDATION_STAGE_AUTHENTICATION,
    VALIDATION_STAGE_REVOCATION,
} validation_stage_t;

static const char *const TAG = "authentication_validation";
static EventGroupHandle_t validation_events;
static QueueHandle_t connection_events;
static device_connection_t *connection;
static device_identity_t *identity_store;
static network_connectivity_t *network;
static uint64_t network_ip_epoch;
static volatile validation_stage_t validation_stage;
static bool pending_challenge_observed;
static bool observed_online;

static void __attribute__((noreturn)) validation_fail(const char *step, int result)
{
    ESP_LOGE(TAG, "step=%s result=%d status=failed", step, result);
    xEventGroupSetBits(validation_events, VALIDATION_FAILED_BIT);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void network_event_callback(
    const network_connectivity_event_t *event,
    void *context)
{
    (void)context;
    if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
        network_ip_epoch = event->ip_epoch;
        xEventGroupSetBits(validation_events, VALIDATION_NETWORK_READY_BIT);
    }
}

static void connection_event_callback(
    const device_connection_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    if (xQueueSend(event_queue, event, 0U) != pdTRUE) {
        xEventGroupSetBits(validation_events, VALIDATION_FAILED_BIT);
    }
}

static void connection_event_task(void *context)
{
    QueueHandle_t event_queue = context;
    device_connection_event_t event;
    while (true) {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.type == DEVICE_CONNECTION_EVENT_FAILURE &&
            event.block_reason != DEVICE_CONNECTION_BLOCK_NONE) {
            validation_fail("device_connection_blocked", (int)event.block_reason);
            continue;
        }
        if (event.type == DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED &&
            validation_stage == VALIDATION_STAGE_PENDING_REJECTION &&
            strcmp(event.message_type, "auth_challenge") == 0) {
            pending_challenge_observed = true;
            continue;
        }
        if (event.type != DEVICE_CONNECTION_EVENT_STATE_CHANGED) {
            continue;
        }
        if (event.state == DEVICE_CONNECTION_STATE_ONLINE) {
            ESP_LOGI(TAG, "connection state=online");
            observed_online = true;
            xEventGroupSetBits(validation_events, VALIDATION_ONLINE_BIT);
        } else if (validation_stage == VALIDATION_STAGE_PENDING_REJECTION &&
                   pending_challenge_observed &&
                   event.state == DEVICE_CONNECTION_STATE_DISCOVERING) {
            ESP_LOGI(TAG, "pending credential rejected and cleared");
            xEventGroupSetBits(validation_events, VALIDATION_PENDING_CLEARED_BIT);
        } else if (validation_stage == VALIDATION_STAGE_REVOCATION &&
                   observed_online &&
                   event.state == DEVICE_CONNECTION_STATE_DISCOVERING) {
            ESP_LOGI(TAG, "connection state=discovering after credential revocation");
            xEventGroupSetBits(validation_events, VALIDATION_REVOKED_BIT);
        }
    }
}

static bool discover_validation_host(host_discovery_record_t *selected)
{
    if (host_discovery_initialize() != HOST_DISCOVERY_RESULT_OK) {
        return false;
    }
    bool found = false;
    for (size_t attempt = 0U; attempt < 6U && !found; ++attempt) {
        host_discovery_record_t records[HOST_DISCOVERY_MAX_RESULTS];
        size_t record_count = 0U;
        if (host_discovery_query(
                5000U,
                records,
                HOST_DISCOVERY_MAX_RESULTS,
                &record_count) != HOST_DISCOVERY_RESULT_OK) {
            continue;
        }
        for (size_t index = 0U; index < record_count; ++index) {
            if (host_discovery_record_is_compatible(&records[index]) &&
                strcmp(records[index].installation_id, VALIDATION_HOST_INSTALLATION_ID) == 0) {
                *selected = records[index];
                found = true;
                break;
            }
        }
    }
    host_discovery_deinitialize();
    return found;
}

static bool save_pending_identity(
    const host_discovery_record_t *host,
    device_connection_identity_t *connection_identity);

static bool prepare_pending_identity(
    const host_discovery_record_t *host,
    device_connection_identity_t *connection_identity)
{
    if (device_identity_open(&identity_store) != DEVICE_IDENTITY_RESULT_OK ||
        device_identity_clear_all(identity_store) != DEVICE_IDENTITY_RESULT_OK) {
        return false;
    }
    return save_pending_identity(host, connection_identity);
}

static bool save_pending_identity(
    const host_discovery_record_t *host,
    device_connection_identity_t *connection_identity)
{
    device_identity_record_t record = {
        .state = DEVICE_IDENTITY_STATE_PENDING,
        .private_seed = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        },
    };
    strcpy(record.device_id, VALIDATION_DEVICE_ID);
    strcpy(record.host_installation_id, host->installation_id);
    strcpy(record.host_certificate_fingerprint, host->certificate_fingerprint);
    if (device_ed25519_derive_public_key(record.private_seed, record.public_key) !=
            DEVICE_CRYPTO_RESULT_OK ||
        device_identity_save_pending(identity_store, &record) !=
            DEVICE_IDENTITY_RESULT_OK) {
        memset(&record, 0, sizeof(record));
        return false;
    }
    connection_identity->paired = true;
    strcpy(connection_identity->installation_id, record.host_installation_id);
    strcpy(
        connection_identity->certificate_fingerprint,
        record.host_certificate_fingerprint);
    memset(&record, 0, sizeof(record));
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting target Device authentication validation");
    esp_err_t storage_result = nvs_flash_init();
    if (storage_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        storage_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        storage_result = nvs_flash_erase();
        if (storage_result == ESP_OK) storage_result = nvs_flash_init();
    }
    validation_events = xEventGroupCreate();
    connection_events = xQueueCreate(
        VALIDATION_EVENT_QUEUE_LENGTH,
        sizeof(device_connection_event_t));
    if (storage_result != ESP_OK || validation_events == NULL || connection_events == NULL ||
        esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) {
        while (true) vTaskDelay(pdMS_TO_TICKS(1000U));
    }
    if (xTaskCreate(
            connection_event_task,
            "authentication_events",
            4096U,
            connection_events,
            5U,
            NULL) != pdPASS) {
        validation_fail("event_task", ESP_ERR_NO_MEM);
    }
    const network_connectivity_config_t network_config = {
        .event_callback = network_event_callback,
    };
    if (network_connectivity_create(&network_config, &network) !=
            NETWORK_CONNECTIVITY_RESULT_OK ||
        network_connectivity_start(network) != NETWORK_CONNECTIVITY_RESULT_OK) {
        validation_fail("network_start", ESP_FAIL);
    }
    EventBits_t bits = xEventGroupWaitBits(
        validation_events,
        VALIDATION_NETWORK_READY_BIT | VALIDATION_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000U));
    if ((bits & VALIDATION_FAILED_BIT) != 0U ||
        (bits & VALIDATION_NETWORK_READY_BIT) == 0U) {
        validation_fail("network_ready", ESP_ERR_TIMEOUT);
    }
    host_discovery_record_t host = {0};
    device_connection_identity_t connection_identity = {0};
    if (!discover_validation_host(&host) ||
        !prepare_pending_identity(&host, &connection_identity)) {
        validation_fail("pending_identity_prepare", ESP_FAIL);
    }
    const device_connection_config_t connection_config = {
        .identity = connection_identity,
        .identity_store = identity_store,
        .capabilities = {
            .input_text = false,
            .input_pcm16_16k_mono = true,
            .output_text = false,
            .output_pcm16_16k_mono = true,
            .playback_cancel = true,
            .display_status = true,
            .display_transcript = false,
        },
        .output_preference = DEVICE_OUTPUT_PREFERENCE_AUDIO,
        .client_version = "authentication-validation",
        .event_callback = connection_event_callback,
        .event_context = connection_events,
    };
    if (device_connection_create(&connection_config, &connection) !=
            DEVICE_CONNECTION_RESULT_OK ||
        device_connection_start(connection) != DEVICE_CONNECTION_RESULT_OK ||
        device_connection_network_changed(connection, true, network_ip_epoch) !=
            DEVICE_CONNECTION_RESULT_OK) {
        validation_fail("device_connection_start", ESP_FAIL);
    }
    bits = xEventGroupWaitBits(
        validation_events,
        VALIDATION_PENDING_CLEARED_BIT | VALIDATION_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000U));
    device_identity_record_t record = {0};
    if ((bits & VALIDATION_FAILED_BIT) != 0U ||
        (bits & VALIDATION_PENDING_CLEARED_BIT) == 0U ||
        device_identity_load(identity_store, &record) != DEVICE_IDENTITY_RESULT_OK ||
        record.state != DEVICE_IDENTITY_STATE_NONE ||
        strcmp(record.host_installation_id, host.installation_id) != 0 ||
        strcmp(record.host_certificate_fingerprint, host.certificate_fingerprint) != 0 ||
        record.device_id[0] != '\0') {
        validation_fail("pending_rejection", ESP_FAIL);
    }
    memset(&record, 0, sizeof(record));
    validation_stage = VALIDATION_STAGE_AUTHENTICATION;
    if (!save_pending_identity(&host, &connection_identity) ||
        device_connection_update_identity(connection, &connection_identity) !=
            DEVICE_CONNECTION_RESULT_OK) {
        validation_fail("pending_retry", ESP_FAIL);
    }
    bits = xEventGroupWaitBits(
        validation_events,
        VALIDATION_ONLINE_BIT | VALIDATION_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000U));
    const device_identity_result_t active_load_result =
        device_identity_load(identity_store, &record);
    ESP_LOGI(
        TAG,
        "active credential check result=%d state=%d event_bits=0x%lx",
        (int)active_load_result,
        (int)record.state,
        (unsigned long)bits);
    if ((bits & VALIDATION_FAILED_BIT) != 0U ||
        (bits & VALIDATION_ONLINE_BIT) == 0U ||
        active_load_result != DEVICE_IDENTITY_RESULT_OK ||
        record.state != DEVICE_IDENTITY_STATE_ACTIVE) {
        validation_fail("pending_activation", ESP_FAIL);
    }
    memset(&record, 0, sizeof(record));
    validation_stage = VALIDATION_STAGE_REVOCATION;
    bits = xEventGroupWaitBits(
        validation_events,
        VALIDATION_REVOKED_BIT | VALIDATION_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(15000U));
    if ((bits & VALIDATION_FAILED_BIT) != 0U ||
        (bits & VALIDATION_REVOKED_BIT) == 0U ||
        device_identity_load(identity_store, &record) != DEVICE_IDENTITY_RESULT_OK ||
        record.state != DEVICE_IDENTITY_STATE_NONE ||
        strcmp(record.host_installation_id, host.installation_id) != 0 ||
        strcmp(record.host_certificate_fingerprint, host.certificate_fingerprint) != 0 ||
        record.device_id[0] != '\0') {
        validation_fail("revoked_identity", ESP_FAIL);
    }
    memset(&record, 0, sizeof(record));
    ESP_LOGI(TAG, "target Device authentication validation passed");
    device_connection_destroy(connection);
    network_connectivity_destroy(network);
    (void)device_identity_clear_all(identity_store);
    device_identity_close(identity_store);
}
