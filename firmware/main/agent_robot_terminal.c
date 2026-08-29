#include <stdbool.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_connectivity.h"
#include "nvs_flash.h"

#define TERMINAL_NETWORK_EVENT_QUEUE_LENGTH 8U
#define TERMINAL_LIFECYCLE_TASK_STACK_BYTES 4096U
#define TERMINAL_LIFECYCLE_TASK_PRIORITY 5U

static const char *const TAG = "terminal_lifecycle";
static QueueHandle_t terminal_network_event_queue;
static network_connectivity_t *terminal_network_connectivity;

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
    if (event_queue != NULL) {
        (void)xQueueSend(event_queue, event, 0U);
    }
}

static void terminal_lifecycle_task(void *context)
{
    QueueHandle_t event_queue = context;
    network_connectivity_event_t event;
    while (true) {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
            esp_ip4_addr_t address = {.addr = event.ipv4_address};
            ESP_LOGI(
                TAG,
                "event=network_ip_acquired state=%s ip=" IPSTR " ip_epoch=%llu",
                network_state_name(event.state),
                IP2STR(&address),
                (unsigned long long)event.ip_epoch);
        } else if (event.type == NETWORK_CONNECTIVITY_EVENT_IP_LOST) {
            ESP_LOGW(
                TAG,
                "event=network_ip_lost state=%s ip_epoch=%llu reason=%u",
                network_state_name(event.state),
                (unsigned long long)event.ip_epoch,
                event.disconnect_reason);
        } else {
            ESP_LOGI(
                TAG,
                "event=network_state_changed state=%s reconnect_delay_ms=%lu",
                network_state_name(event.state),
                (unsigned long)event.reconnect_delay_ms);
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

    terminal_network_event_queue = xQueueCreate(
        TERMINAL_NETWORK_EVENT_QUEUE_LENGTH,
        sizeof(network_connectivity_event_t));
    ESP_ERROR_CHECK(terminal_network_event_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(
        xTaskCreate(
            terminal_lifecycle_task,
            "terminal_lifecycle",
            TERMINAL_LIFECYCLE_TASK_STACK_BYTES,
            terminal_network_event_queue,
            TERMINAL_LIFECYCLE_TASK_PRIORITY,
            NULL)
                == pdPASS
            ? ESP_OK
            : ESP_ERR_NO_MEM);

    const network_connectivity_config_t network_config = {
        .event_callback = terminal_network_event,
        .event_context = terminal_network_event_queue,
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
    ESP_LOGI(TAG, "event=terminal_boot status=network_started");
}
