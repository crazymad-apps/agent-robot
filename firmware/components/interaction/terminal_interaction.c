#include "terminal_interaction.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "agent_robot_board.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "terminal_button_policy.h"
#include "terminal_interaction_policy.h"

#define TERMINAL_INTERACTION_SAMPLE_PERIOD_MS 5U
#define TERMINAL_INTERACTION_TASK_STACK_BYTES 3072U
#define TERMINAL_INTERACTION_TASK_PRIORITY 5U
#define TERMINAL_INTERACTION_STOPPED_BIT BIT0

struct terminal_interaction {
    bool boot_action_evaluated;
    bool started;
    bool stop_requested;
    TaskHandle_t task;
    EventGroupHandle_t lifecycle_events;
    terminal_interaction_event_callback_t event_callback;
    void *event_context;
    portMUX_TYPE lock;
};

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static bool button_pressed(void)
{
    return gpio_get_level(AGENT_ROBOT_BOARD_PTT_GPIO) == 0;
}

terminal_interaction_result_t terminal_interaction_create(
    terminal_interaction_t **interaction)
{
    if (interaction == NULL || *interaction != NULL) {
        return TERMINAL_INTERACTION_RESULT_INVALID_ARGUMENT;
    }
    terminal_interaction_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return TERMINAL_INTERACTION_RESULT_NO_MEMORY;
    }
    created->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    created->lifecycle_events = xEventGroupCreate();
    if (created->lifecycle_events == NULL) {
        free(created);
        return TERMINAL_INTERACTION_RESULT_NO_MEMORY;
    }
    const gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << AGENT_ROBOT_BOARD_PTT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&input_config) != ESP_OK) {
        vEventGroupDelete(created->lifecycle_events);
        free(created);
        return TERMINAL_INTERACTION_RESULT_HARDWARE_FAILURE;
    }
    *interaction = created;
    return TERMINAL_INTERACTION_RESULT_OK;
}

terminal_interaction_result_t terminal_interaction_evaluate_boot_action(
    terminal_interaction_t *interaction,
    terminal_interaction_boot_callback_t callback,
    void *callback_context,
    terminal_interaction_boot_action_t *action)
{
    if (interaction == NULL || action == NULL || interaction->boot_action_evaluated) {
        return TERMINAL_INTERACTION_RESULT_INVALID_ARGUMENT;
    }
    interaction->boot_action_evaluated = true;
    *action = TERMINAL_INTERACTION_BOOT_ACTION_NONE;

    terminal_interaction_policy_t policy;
    terminal_interaction_policy_initialize(
        &policy,
        monotonic_ms(),
        button_pressed());
    while (true) {
        const terminal_interaction_policy_state_t previous_state = policy.state;
        const terminal_interaction_policy_state_t state =
            terminal_interaction_policy_update(
                &policy,
                monotonic_ms(),
                button_pressed());
        if (state == TERMINAL_INTERACTION_POLICY_CANCELLED) {
            return TERMINAL_INTERACTION_RESULT_OK;
        }
        if (state == TERMINAL_INTERACTION_POLICY_CONFIRMED) {
            *action = TERMINAL_INTERACTION_BOOT_ACTION_CLEAR_PAIRING;
            return TERMINAL_INTERACTION_RESULT_OK;
        }
        if (state == TERMINAL_INTERACTION_POLICY_ARMED &&
            previous_state != TERMINAL_INTERACTION_POLICY_ARMED) {
            if (callback != NULL) {
                callback(
                    TERMINAL_INTERACTION_BOOT_MILESTONE_PAIRING_RESET_ARMED,
                    callback_context);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TERMINAL_INTERACTION_SAMPLE_PERIOD_MS));
    }
}

static void terminal_interaction_task(void *context)
{
    terminal_interaction_t *interaction = context;
    terminal_button_policy_t policy;
    terminal_button_policy_initialize(&policy, monotonic_ms(), button_pressed());
    while (true) {
        taskENTER_CRITICAL(&interaction->lock);
        const bool stop_requested = interaction->stop_requested;
        terminal_interaction_event_callback_t callback = interaction->event_callback;
        void *callback_context = interaction->event_context;
        taskEXIT_CRITICAL(&interaction->lock);
        if (stop_requested) {
            break;
        }
        const uint64_t now_ms = monotonic_ms();
        bool stable_pressed = false;
        if (terminal_button_policy_update(
                &policy,
                now_ms,
                button_pressed(),
                &stable_pressed) &&
            callback != NULL) {
            const terminal_interaction_event_t event = {
                .type = stable_pressed ? TERMINAL_INTERACTION_EVENT_PTT_PRESSED
                                       : TERMINAL_INTERACTION_EVENT_PTT_RELEASED,
                .occurred_at_ms = now_ms,
            };
            callback(&event, callback_context);
        }
        vTaskDelay(pdMS_TO_TICKS(TERMINAL_INTERACTION_SAMPLE_PERIOD_MS));
    }
    taskENTER_CRITICAL(&interaction->lock);
    interaction->started = false;
    interaction->stop_requested = false;
    interaction->task = NULL;
    interaction->event_callback = NULL;
    interaction->event_context = NULL;
    taskEXIT_CRITICAL(&interaction->lock);
    xEventGroupSetBits(interaction->lifecycle_events, TERMINAL_INTERACTION_STOPPED_BIT);
    vTaskDelete(NULL);
}

terminal_interaction_result_t terminal_interaction_start(
    terminal_interaction_t *interaction,
    terminal_interaction_event_callback_t callback,
    void *callback_context)
{
    if (interaction == NULL || callback == NULL) {
        return TERMINAL_INTERACTION_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&interaction->lock);
    if (!interaction->boot_action_evaluated || interaction->started) {
        taskEXIT_CRITICAL(&interaction->lock);
        return TERMINAL_INTERACTION_RESULT_INVALID_STATE;
    }
    interaction->started = true;
    interaction->stop_requested = false;
    interaction->event_callback = callback;
    interaction->event_context = callback_context;
    xEventGroupClearBits(interaction->lifecycle_events, TERMINAL_INTERACTION_STOPPED_BIT);
    taskEXIT_CRITICAL(&interaction->lock);
    const BaseType_t task_result = xTaskCreate(
        terminal_interaction_task,
        "terminal_button",
        TERMINAL_INTERACTION_TASK_STACK_BYTES,
        interaction,
        TERMINAL_INTERACTION_TASK_PRIORITY,
        &interaction->task);
    if (task_result != pdPASS) {
        taskENTER_CRITICAL(&interaction->lock);
        interaction->started = false;
        interaction->event_callback = NULL;
        interaction->event_context = NULL;
        taskEXIT_CRITICAL(&interaction->lock);
        return TERMINAL_INTERACTION_RESULT_NO_MEMORY;
    }
    return TERMINAL_INTERACTION_RESULT_OK;
}

void terminal_interaction_stop(terminal_interaction_t *interaction)
{
    if (interaction == NULL) {
        return;
    }
    taskENTER_CRITICAL(&interaction->lock);
    if (!interaction->started) {
        taskEXIT_CRITICAL(&interaction->lock);
        return;
    }
    interaction->stop_requested = true;
    taskEXIT_CRITICAL(&interaction->lock);
    (void)xEventGroupWaitBits(
        interaction->lifecycle_events,
        TERMINAL_INTERACTION_STOPPED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);
}

void terminal_interaction_destroy(terminal_interaction_t *interaction)
{
    if (interaction == NULL) {
        return;
    }
    terminal_interaction_stop(interaction);
    (void)gpio_reset_pin(AGENT_ROBOT_BOARD_PTT_GPIO);
    vEventGroupDelete(interaction->lifecycle_events);
    free(interaction);
}
