#include "terminal_button_policy.h"

#include <stddef.h>

void terminal_button_policy_initialize(
    terminal_button_policy_t *policy,
    uint64_t now_ms,
    bool pressed)
{
    if (policy == NULL) {
        return;
    }
    *policy = (terminal_button_policy_t){
        .candidate_pressed = pressed,
        .stable_pressed = pressed,
        .candidate_since_ms = now_ms,
        .last_update_ms = now_ms,
    };
}

bool terminal_button_policy_update(
    terminal_button_policy_t *policy,
    uint64_t now_ms,
    bool pressed,
    bool *stable_pressed)
{
    if (policy == NULL || stable_pressed == NULL || now_ms < policy->last_update_ms) {
        return false;
    }
    policy->last_update_ms = now_ms;
    if (pressed != policy->candidate_pressed) {
        policy->candidate_pressed = pressed;
        policy->candidate_since_ms = now_ms;
    }
    if (policy->candidate_pressed == policy->stable_pressed ||
        now_ms - policy->candidate_since_ms < TERMINAL_BUTTON_DEBOUNCE_MS) {
        return false;
    }
    policy->stable_pressed = policy->candidate_pressed;
    *stable_pressed = policy->stable_pressed;
    return true;
}
