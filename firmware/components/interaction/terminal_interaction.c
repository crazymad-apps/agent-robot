#include "terminal_interaction.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "agent_robot_board.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "terminal_interaction_policy.h"

#define TERMINAL_INTERACTION_SAMPLE_PERIOD_MS 5U

struct terminal_interaction {
    bool boot_action_evaluated;
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
    const gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << AGENT_ROBOT_BOARD_PTT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&input_config) != ESP_OK) {
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

void terminal_interaction_destroy(terminal_interaction_t *interaction)
{
    if (interaction == NULL) {
        return;
    }
    (void)gpio_reset_pin(AGENT_ROBOT_BOARD_PTT_GPIO);
    free(interaction);
}
