#include <stdbool.h>
#include <string.h>

#include "device_identity.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "device_connection.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_connectivity.h"
#include "nvs_flash.h"

#define TERMINAL_EVENT_QUEUE_LENGTH 16U
#define TERMINAL_LIFECYCLE_TASK_STACK_BYTES 4096U
#define TERMINAL_LIFECYCLE_TASK_PRIORITY 5U

static const char *const TAG = "terminal_lifecycle";
typedef enum {
    TERMINAL_EVENT_NETWORK = 0,
    TERMINAL_EVENT_DEVICE_CONNECTION,
} terminal_event_type_t;

typedef struct {
    terminal_event_type_t type;
    union {
        network_connectivity_event_t network;
        device_connection_event_t device_connection;
    } value;
} terminal_event_t;

static QueueHandle_t terminal_event_queue;
static network_connectivity_t *terminal_network_connectivity;
static device_connection_t *terminal_device_connection;
static device_identity_t *terminal_device_identity;
static uint32_t terminal_event_drop_count;
static uint32_t terminal_device_network_update_error_count;

static void terminal_device_connection_network_changed(
    bool has_ip,
    uint64_t ip_epoch);

static const char *network_state_name(network_connectivity_state_t state)
{
    switch (state) {
        case NETWORK_CONNECTIVITY_STATE_STOPPED:
            return "stopped";
        case NETWORK_CONNECTIVITY_STATE_UNCONFIGURED:
            return "unconfigured";
        case NETWORK_CONNECTIVITY_STATE_CONNECTING:
            return "connecting";
        case NETWORK_CONNECTIVITY_STATE_READY:
            return "ready";
        case NETWORK_CONNECTIVITY_STATE_BACKOFF:
            return "backoff";
        case NETWORK_CONNECTIVITY_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static void terminal_network_event(
    const network_connectivity_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
        terminal_device_connection_network_changed(true, event->ip_epoch);
    } else if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_LOST) {
        terminal_device_connection_network_changed(false, event->ip_epoch);
    }
    const terminal_event_t terminal_event = {
        .type = TERMINAL_EVENT_NETWORK,
        .value.network = *event,
    };
    if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
        (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
    }
}

static void terminal_device_connection_event(
    const device_connection_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    const terminal_event_t terminal_event = {
        .type = TERMINAL_EVENT_DEVICE_CONNECTION,
        .value.device_connection = *event,
    };
    if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
        (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
    }
}

static const char *device_connection_state_name(device_connection_state_t state)
{
    switch (state) {
        case DEVICE_CONNECTION_STATE_WIFI_DOWN:
            return "wifi_down";
        case DEVICE_CONNECTION_STATE_DISCOVERING:
            return "discovering";
        case DEVICE_CONNECTION_STATE_CONNECTING:
            return "connecting";
        case DEVICE_CONNECTION_STATE_AUTHENTICATING:
            return "authenticating";
        case DEVICE_CONNECTION_STATE_ONLINE:
            return "online";
        case DEVICE_CONNECTION_STATE_BACKOFF:
            return "backoff";
        case DEVICE_CONNECTION_STATE_BLOCKED:
            return "blocked";
        default:
            return "unknown";
    }
}

static void terminal_device_connection_event_process(
    const device_connection_event_t *event)
{
    if (event->type == DEVICE_CONNECTION_EVENT_HOST_SELECTED) {
        ESP_LOGI(
            TAG,
            "event=device_host_selected state=%s host_suffix=%s connection_epoch=%llu",
            device_connection_state_name(event->state),
            event->installation_id_suffix,
            (unsigned long long)event->connection_epoch);
    } else if (event->type == DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED) {
        ESP_LOGI(
            TAG,
            "event=device_control_received state=%s type=%s connection_epoch=%llu",
            device_connection_state_name(event->state),
            event->message_type,
            (unsigned long long)event->connection_epoch);
    } else if (event->type == DEVICE_CONNECTION_EVENT_FAILURE) {
        ESP_LOGW(
            TAG,
            "event=device_connection_failure state=%s failure=%u selection=%u transport=%u "
            "block=%u reconnect_delay_ms=%lu connection_epoch=%llu",
            device_connection_state_name(event->state),
            (unsigned int)event->failure,
            (unsigned int)event->selection_result,
            (unsigned int)event->transport_result,
            (unsigned int)event->block_reason,
            (unsigned long)event->reconnect_delay_ms,
            (unsigned long long)event->connection_epoch);
    } else {
        ESP_LOGI(
            TAG,
            "event=device_connection_state_changed state=%s block=%u connection_epoch=%llu",
            device_connection_state_name(event->state),
            (unsigned int)event->block_reason,
            (unsigned long long)event->connection_epoch);
    }
}

static void terminal_device_connection_network_changed(
    bool has_ip,
    uint64_t ip_epoch)
{
    if (terminal_device_connection == NULL) {
        return;
    }
    const device_connection_result_t result = device_connection_network_changed(
        terminal_device_connection,
        has_ip,
        ip_epoch);
    if (result != DEVICE_CONNECTION_RESULT_OK) {
        (void)__atomic_fetch_add(
            &terminal_device_network_update_error_count,
            1U,
            __ATOMIC_RELAXED);
    }
}

static void terminal_secure_zero(void *memory, size_t memory_size)
{
    volatile uint8_t *bytes = memory;
    while (memory_size > 0U) {
        *bytes++ = 0U;
        --memory_size;
    }
}

static bool terminal_identity_load(
    device_connection_identity_t *connection_identity)
{
    device_identity_record_t identity_record = {0};
    const device_identity_result_t result = device_identity_load(
        terminal_device_identity,
        &identity_record);
    if (result == DEVICE_IDENTITY_RESULT_NOT_FOUND) {
        ESP_LOGI(TAG, "event=device_identity_loaded state=none host_binding_retained=false");
        return true;
    }
    if (result != DEVICE_IDENTITY_RESULT_OK) {
        ESP_LOGE(
            TAG,
            "event=device_identity_load_failed result=%u connection_service=disabled",
            (unsigned int)result);
        terminal_secure_zero(&identity_record, sizeof(identity_record));
        return false;
    }
    if (identity_record.state == DEVICE_IDENTITY_STATE_NONE) {
        ESP_LOGI(
            TAG,
            "event=device_identity_loaded state=none host_binding_retained=%s",
            identity_record.host_installation_id[0] == '\0' ? "false" : "true");
        terminal_secure_zero(&identity_record, sizeof(identity_record));
        return true;
    }
    connection_identity->paired = true;
    memcpy(
        connection_identity->installation_id,
        identity_record.host_installation_id,
        sizeof(connection_identity->installation_id));
    memcpy(
        connection_identity->certificate_fingerprint,
        identity_record.host_certificate_fingerprint,
        sizeof(connection_identity->certificate_fingerprint));
    ESP_LOGI(
        TAG,
        "event=device_identity_loaded state=%s",
        identity_record.state == DEVICE_IDENTITY_STATE_ACTIVE ? "active" : "pending");
    terminal_secure_zero(&identity_record, sizeof(identity_record));
    return true;
}

static void terminal_lifecycle_task(void *context)
{
    QueueHandle_t event_queue = context;
    terminal_event_t event;
    while (true) {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const uint32_t dropped =
            __atomic_exchange_n(&terminal_event_drop_count, 0U, __ATOMIC_RELAXED);
        if (dropped != 0U) {
            ESP_LOGE(TAG, "event=terminal_event_queue_overflow dropped=%lu", (unsigned long)dropped);
        }
        const uint32_t network_update_errors = __atomic_exchange_n(
            &terminal_device_network_update_error_count,
            0U,
            __ATOMIC_RELAXED);
        if (network_update_errors != 0U) {
            ESP_LOGE(
                TAG,
                "event=device_network_update_failed count=%lu",
                (unsigned long)network_update_errors);
        }
        if (event.type == TERMINAL_EVENT_DEVICE_CONNECTION) {
            terminal_device_connection_event_process(&event.value.device_connection);
            continue;
        }
        const network_connectivity_event_t *network_event = &event.value.network;
        if (network_event->type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
            esp_ip4_addr_t address = {.addr = network_event->ipv4_address};
            ESP_LOGI(
                TAG,
                "event=network_ip_acquired state=%s ip=" IPSTR " ip_epoch=%llu",
                network_state_name(network_event->state),
                IP2STR(&address),
                (unsigned long long)network_event->ip_epoch);
        } else if (network_event->type == NETWORK_CONNECTIVITY_EVENT_IP_LOST) {
            ESP_LOGW(
                TAG,
                "event=network_ip_lost state=%s ip_epoch=%llu reason=%u",
                network_state_name(network_event->state),
                (unsigned long long)network_event->ip_epoch,
                network_event->disconnect_reason);
        } else {
            ESP_LOGI(
                TAG,
                "event=network_state_changed state=%s reconnect_delay_ms=%lu",
                network_state_name(network_event->state),
                (unsigned long)network_event->reconnect_delay_ms);
        }
    }
}

static void terminal_storage_initialize(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

void app_main(void)
{
    terminal_storage_initialize();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    terminal_event_queue = xQueueCreate(
        TERMINAL_EVENT_QUEUE_LENGTH,
        sizeof(terminal_event_t));
    ESP_ERROR_CHECK(terminal_event_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(
        xTaskCreate(
            terminal_lifecycle_task,
            "terminal_lifecycle",
            TERMINAL_LIFECYCLE_TASK_STACK_BYTES,
            terminal_event_queue,
            TERMINAL_LIFECYCLE_TASK_PRIORITY,
            NULL)
                == pdPASS
            ? ESP_OK
            : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(
        device_identity_open(&terminal_device_identity) == DEVICE_IDENTITY_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    device_connection_identity_t connection_identity = {0};
    const bool identity_available = terminal_identity_load(&connection_identity);
    if (identity_available) {
        const device_connection_config_t device_connection_config = {
            .identity = connection_identity,
            .event_callback = terminal_device_connection_event,
            .event_context = terminal_event_queue,
        };
        ESP_ERROR_CHECK(
            device_connection_create(
                &device_connection_config,
                &terminal_device_connection) == DEVICE_CONNECTION_RESULT_OK
                ? ESP_OK
                : ESP_FAIL);
        ESP_ERROR_CHECK(
            device_connection_start(terminal_device_connection) == DEVICE_CONNECTION_RESULT_OK
                ? ESP_OK
                : ESP_FAIL);
    }

    const network_connectivity_config_t network_config = {
        .event_callback = terminal_network_event,
        .event_context = terminal_event_queue,
    };
    ESP_ERROR_CHECK(
        network_connectivity_create(
            &network_config,
            &terminal_network_connectivity)
                == NETWORK_CONNECTIVITY_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    ESP_ERROR_CHECK(
        network_connectivity_start(terminal_network_connectivity)
                == NETWORK_CONNECTIVITY_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    ESP_LOGI(
        TAG,
        "event=terminal_boot status=%s",
        identity_available ? "connection_services_started" : "identity_recovery_required");
}
