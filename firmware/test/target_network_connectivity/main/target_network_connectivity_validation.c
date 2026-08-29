#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "network_connectivity.h"
#include "nvs_flash.h"

#define INITIAL_IP_ACQUIRED_BIT BIT0
#define IP_LOST_BIT BIT1
#define RECOVERED_IP_ACQUIRED_BIT BIT2
#define INITIAL_BACKOFF_OBSERVED_BIT BIT3

typedef struct {
    EventGroupHandle_t events;
    uint32_t acquisition_count;
    network_connectivity_event_t last_ip_lost;
} validation_context_t;

static const char *const TAG = "network_validation";

static void validation_fail(const char *step, int result)
{
    ESP_LOGE(TAG, "step=%s result=%d status=failed", step, result);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void require_validation(bool condition, const char *step)
{
    if (!condition) {
        validation_fail(step, ESP_FAIL);
    }
}

static void connectivity_event(
    const network_connectivity_event_t *event,
    void *context)
{
    validation_context_t *validation = context;
    if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
        ++validation->acquisition_count;
        xEventGroupSetBits(
            validation->events,
            validation->acquisition_count == 1U
                ? INITIAL_IP_ACQUIRED_BIT
                : RECOVERED_IP_ACQUIRED_BIT);
    } else if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_LOST) {
        validation->last_ip_lost = *event;
        xEventGroupSetBits(validation->events, IP_LOST_BIT);
    } else if (
        event->type == NETWORK_CONNECTIVITY_EVENT_STATE_CHANGED
        && event->state == NETWORK_CONNECTIVITY_STATE_BACKOFF
        && event->reconnect_delay_ms == 1000U) {
        xEventGroupSetBits(validation->events, INITIAL_BACKOFF_OBSERVED_BIT);
    }
}

static void wait_for_bits(EventGroupHandle_t events, EventBits_t bits, uint32_t timeout_ms)
{
    const EventBits_t observed = xEventGroupWaitBits(
        events, bits, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    require_validation((observed & bits) == bits, "event_wait");
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting target Wi-Fi lifecycle validation");
    require_validation(nvs_flash_init() == ESP_OK, "nvs_initialize");
    require_validation(esp_netif_init() == ESP_OK, "network_interface_initialize");
    require_validation(
        esp_event_loop_create_default() == ESP_OK,
        "event_loop_initialize");

    validation_context_t validation = {
        .events = xEventGroupCreate(),
    };
    require_validation(validation.events != NULL, "event_group_create");

    const network_connectivity_config_t config = {
        .event_callback = connectivity_event,
        .event_context = &validation,
    };
    network_connectivity_t *connectivity = NULL;
    require_validation(
        network_connectivity_create(&config, &connectivity)
            == NETWORK_CONNECTIVITY_RESULT_OK,
        "connectivity_create");
    require_validation(
        network_connectivity_start(connectivity)
            == NETWORK_CONNECTIVITY_RESULT_OK,
        "connectivity_start");

    wait_for_bits(validation.events, INITIAL_IP_ACQUIRED_BIT, 30000U);
    network_connectivity_status_t initial_status;
    network_connectivity_get_status(connectivity, &initial_status);
    require_validation(
        initial_status.state == NETWORK_CONNECTIVITY_STATE_READY
            && initial_status.has_credentials
            && initial_status.has_ip
            && initial_status.ip_epoch != 0U
            && initial_status.ipv4_address != 0U,
        "initial_ready_status");

    xEventGroupClearBits(
        validation.events,
        IP_LOST_BIT | RECOVERED_IP_ACQUIRED_BIT | INITIAL_BACKOFF_OBSERVED_BIT);
    const int64_t disconnect_time_us = esp_timer_get_time();
    require_validation(esp_wifi_disconnect() == ESP_OK, "disconnect_injection");
    wait_for_bits(
        validation.events,
        IP_LOST_BIT | INITIAL_BACKOFF_OBSERVED_BIT,
        5000U);
    wait_for_bits(validation.events, RECOVERED_IP_ACQUIRED_BIT, 15000U);
    const uint32_t recovery_time_ms =
        (uint32_t)((esp_timer_get_time() - disconnect_time_us) / 1000);

    network_connectivity_status_t recovered_status;
    network_connectivity_get_status(connectivity, &recovered_status);
    require_validation(
        recovered_status.state == NETWORK_CONNECTIVITY_STATE_READY
            && recovered_status.has_ip
            && recovered_status.ip_epoch > initial_status.ip_epoch
            && recovered_status.disconnect_count >= 1U
            && recovered_status.reconnect_attempt == 0U
            && recovered_status.current_reconnect_delay_ms == 0U,
        "recovered_ready_status");
    require_validation(
        validation.last_ip_lost.state == NETWORK_CONNECTIVITY_STATE_READY
            && validation.last_ip_lost.ip_epoch == initial_status.ip_epoch,
        "link_loss_event_snapshot");

    ESP_LOGI(
        TAG,
        "recovered automatically initial_epoch=%llu recovered_epoch=%llu elapsed_ms=%lu",
        (unsigned long long)initial_status.ip_epoch,
        (unsigned long long)recovered_status.ip_epoch,
        (unsigned long)recovery_time_ms);

    xEventGroupClearBits(validation.events, IP_LOST_BIT);
    network_connectivity_stop(connectivity);
    wait_for_bits(validation.events, IP_LOST_BIT, 1000U);
    network_connectivity_status_t stopped_status;
    network_connectivity_get_status(connectivity, &stopped_status);
    require_validation(
        stopped_status.state == NETWORK_CONNECTIVITY_STATE_STOPPED
            && !stopped_status.has_ip
            && validation.last_ip_lost.state == NETWORK_CONNECTIVITY_STATE_STOPPED,
        "stopped_status");

    network_connectivity_destroy(connectivity);
    vEventGroupDelete(validation.events);
    ESP_LOGI(TAG, "target network connectivity validation passed");
}
