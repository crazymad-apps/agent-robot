#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "device_connection.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_connectivity.h"
#include "nvs_flash.h"

#define VALIDATION_EVENT_QUEUE_LENGTH 32U
#define VALIDATION_LIFECYCLE_TASK_STACK_BYTES 4096U
#define VALIDATION_LIFECYCLE_TASK_PRIORITY 5U
#define VALIDATION_COMPLETE_BIT BIT0
#define VALIDATION_FAILED_BIT BIT1

static const char *const TAG = "connection_recovery_validation";
static device_connection_t *device_connection;
static network_connectivity_t *network_connectivity;
static QueueHandle_t validation_event_queue;
static EventGroupHandle_t validation_events;
static uint32_t dropped_event_count;
static uint32_t network_update_error_count;

static void validation_fail(const char *step, int result)
{
    ESP_LOGE(TAG, "step=%s result=%d status=failed", step, result);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void device_event_callback(
    const device_connection_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    if (xQueueSend(event_queue, event, 0U) != pdTRUE) {
        (void)__atomic_fetch_add(&dropped_event_count, 1U, __ATOMIC_RELAXED);
    }
}

static void network_event_callback(
    const network_connectivity_event_t *event,
    void *context)
{
    (void)context;
    device_connection_result_t result = DEVICE_CONNECTION_RESULT_OK;
    if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
        result = device_connection_network_changed(
            device_connection,
            true,
            event->ip_epoch);
    } else if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_LOST) {
        result = device_connection_network_changed(
            device_connection,
            false,
            event->ip_epoch);
    }
    if (result != DEVICE_CONNECTION_RESULT_OK) {
        (void)__atomic_fetch_add(&network_update_error_count, 1U, __ATOMIC_RELAXED);
    }
}

static void validation_lifecycle_task(void *context)
{
    QueueHandle_t event_queue = context;
    uint32_t challenge_count = 0U;
    uint32_t host_selection_count = 0U;
    uint64_t first_challenge_epoch = 0U;
    device_connection_event_t event;
    while (true) {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.type == DEVICE_CONNECTION_EVENT_HOST_SELECTED) {
            ++host_selection_count;
            ESP_LOGI(
                TAG,
                "event=host_selected count=%lu connection_epoch=%llu",
                (unsigned long)host_selection_count,
                (unsigned long long)event.connection_epoch);
            continue;
        }
        if (event.type == DEVICE_CONNECTION_EVENT_FAILURE) {
            ESP_LOGW(
                TAG,
                "event=connection_failure failure=%u transport=%u delay_ms=%lu "
                "connection_epoch=%llu",
                (unsigned int)event.failure,
                (unsigned int)event.transport_result,
                (unsigned long)event.reconnect_delay_ms,
                (unsigned long long)event.connection_epoch);
            if (event.block_reason != DEVICE_CONNECTION_BLOCK_NONE) {
                xEventGroupSetBits(validation_events, VALIDATION_FAILED_BIT);
            }
            continue;
        }
        if (event.type != DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED ||
            strcmp(event.message_type, "auth_challenge") != 0) {
            continue;
        }
        ++challenge_count;
        ESP_LOGI(
            TAG,
            "event=auth_challenge count=%lu connection_epoch=%llu",
            (unsigned long)challenge_count,
            (unsigned long long)event.connection_epoch);
        if (challenge_count == 1U) {
            first_challenge_epoch = event.connection_epoch;
        } else if (challenge_count == 2U) {
            if (event.connection_epoch <= first_challenge_epoch || host_selection_count < 2U) {
                xEventGroupSetBits(validation_events, VALIDATION_FAILED_BIT);
            } else {
                xEventGroupSetBits(validation_events, VALIDATION_COMPLETE_BIT);
            }
        }
    }
}

static void storage_initialize(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result == ESP_OK) {
            result = nvs_flash_init();
        }
    }
    if (result != ESP_OK) {
        validation_fail("storage_initialize", result);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting target Device Connection recovery validation");
    storage_initialize();
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) {
        validation_fail("network_platform_initialize", ESP_FAIL);
    }
    validation_event_queue = xQueueCreate(
        VALIDATION_EVENT_QUEUE_LENGTH,
        sizeof(device_connection_event_t));
    validation_events = xEventGroupCreate();
    if (validation_event_queue == NULL || validation_events == NULL) {
        validation_fail("validation_resources", ESP_ERR_NO_MEM);
    }
    if (xTaskCreate(
            validation_lifecycle_task,
            "connection_recovery_events",
            VALIDATION_LIFECYCLE_TASK_STACK_BYTES,
            validation_event_queue,
            VALIDATION_LIFECYCLE_TASK_PRIORITY,
            NULL) != pdPASS) {
        validation_fail("validation_task", ESP_ERR_NO_MEM);
    }

    const device_connection_config_t device_config = {
        .identity = {.paired = false},
        .event_callback = device_event_callback,
        .event_context = validation_event_queue,
    };
    if (device_connection_create(&device_config, &device_connection) !=
            DEVICE_CONNECTION_RESULT_OK ||
        device_connection_start(device_connection) != DEVICE_CONNECTION_RESULT_OK) {
        validation_fail("device_connection_start", ESP_FAIL);
    }
    const network_connectivity_config_t network_config = {
        .event_callback = network_event_callback,
    };
    if (network_connectivity_create(&network_config, &network_connectivity) !=
            NETWORK_CONNECTIVITY_RESULT_OK ||
        network_connectivity_start(network_connectivity) != NETWORK_CONNECTIVITY_RESULT_OK) {
        validation_fail("network_connectivity_start", ESP_FAIL);
    }

    const EventBits_t result = xEventGroupWaitBits(
        validation_events,
        VALIDATION_COMPLETE_BIT | VALIDATION_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(90000U));
    if ((result & VALIDATION_FAILED_BIT) != 0U ||
        (result & VALIDATION_COMPLETE_BIT) == 0U) {
        validation_fail("connection_recovery", ESP_ERR_TIMEOUT);
    }
    device_connection_status_t status;
    device_connection_get_status(device_connection, &status);
    const uint32_t dropped = __atomic_load_n(&dropped_event_count, __ATOMIC_RELAXED);
    const uint32_t network_update_errors =
        __atomic_load_n(&network_update_error_count, __ATOMIC_RELAXED);
    if (status.discovery_attempt_count < 2U || status.transport_attempt_count < 3U ||
        status.control_message_count != 2U || dropped != 0U || network_update_errors != 0U) {
        validation_fail("connection_recovery_boundaries", ESP_FAIL);
    }

    ESP_LOGI(
        TAG,
        "target Device Connection recovery validation passed discovery_attempts=%lu "
        "transport_attempts=%lu connection_epoch=%llu",
        (unsigned long)status.discovery_attempt_count,
        (unsigned long)status.transport_attempt_count,
        (unsigned long long)status.connection_epoch);
    network_connectivity_destroy(network_connectivity);
    device_connection_destroy(device_connection);
}
