#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TERMINAL_INTERACTION_DEBOUNCE_MS 30U
#define TERMINAL_INTERACTION_PAIRING_RESET_HOLD_MS 8000U

typedef enum {
    TERMINAL_INTERACTION_POLICY_EVALUATING = 0,
    TERMINAL_INTERACTION_POLICY_HOLDING,
    TERMINAL_INTERACTION_POLICY_ARMED,
    TERMINAL_INTERACTION_POLICY_CANCELLED,
    TERMINAL_INTERACTION_POLICY_CONFIRMED,
} terminal_interaction_policy_state_t;

typedef struct {
    terminal_interaction_policy_state_t state;
    bool candidate_pressed;
    bool stable_pressed;
    uint64_t candidate_since_ms;
    uint64_t hold_started_ms;
    uint64_t last_update_ms;
} terminal_interaction_policy_t;

void terminal_interaction_policy_initialize(
    terminal_interaction_policy_t *policy,
    uint64_t now_ms,
    bool pressed);

terminal_interaction_policy_state_t terminal_interaction_policy_update(
    terminal_interaction_policy_t *policy,
    uint64_t now_ms,
    bool pressed);
