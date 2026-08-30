#include "terminal_interaction_policy.h"

#include <stddef.h>

void terminal_interaction_policy_initialize(
    terminal_interaction_policy_t *policy,
    uint64_t now_ms,
    bool pressed)
{
    if (policy == NULL) {
        return;
    }
    *policy = (terminal_interaction_policy_t){
        .state = TERMINAL_INTERACTION_POLICY_EVALUATING,
        .candidate_pressed = pressed,
        .stable_pressed = pressed,
        .candidate_since_ms = now_ms,
        .last_update_ms = now_ms,
    };
}

terminal_interaction_policy_state_t terminal_interaction_policy_update(
    terminal_interaction_policy_t *policy,
    uint64_t now_ms,
    bool pressed)
{
    if (policy == NULL || now_ms < policy->last_update_ms ||
        policy->state == TERMINAL_INTERACTION_POLICY_CANCELLED ||
        policy->state == TERMINAL_INTERACTION_POLICY_CONFIRMED) {
        return policy == NULL ? TERMINAL_INTERACTION_POLICY_CANCELLED : policy->state;
    }
    policy->last_update_ms = now_ms;
    if (pressed != policy->candidate_pressed) {
        policy->candidate_pressed = pressed;
        policy->candidate_since_ms = now_ms;
    }

    if (policy->state == TERMINAL_INTERACTION_POLICY_EVALUATING) {
        if (now_ms - policy->candidate_since_ms < TERMINAL_INTERACTION_DEBOUNCE_MS) {
            return policy->state;
        }
        policy->stable_pressed = policy->candidate_pressed;
        if (!policy->stable_pressed) {
            policy->state = TERMINAL_INTERACTION_POLICY_CANCELLED;
            return policy->state;
        }
        policy->hold_started_ms = policy->candidate_since_ms;
        policy->state = TERMINAL_INTERACTION_POLICY_HOLDING;
    }

    if (policy->candidate_pressed != policy->stable_pressed &&
        now_ms - policy->candidate_since_ms >= TERMINAL_INTERACTION_DEBOUNCE_MS) {
        policy->stable_pressed = policy->candidate_pressed;
        if (!policy->stable_pressed) {
            policy->state = policy->state == TERMINAL_INTERACTION_POLICY_ARMED
                                ? TERMINAL_INTERACTION_POLICY_CONFIRMED
                                : TERMINAL_INTERACTION_POLICY_CANCELLED;
            return policy->state;
        }
    }
    if (policy->state == TERMINAL_INTERACTION_POLICY_HOLDING &&
        policy->candidate_pressed &&
        now_ms - policy->hold_started_ms >=
            TERMINAL_INTERACTION_PAIRING_RESET_HOLD_MS) {
        policy->state = TERMINAL_INTERACTION_POLICY_ARMED;
    }
    return policy->state;
}
