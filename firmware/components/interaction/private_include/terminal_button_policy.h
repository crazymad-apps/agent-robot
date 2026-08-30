#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TERMINAL_BUTTON_DEBOUNCE_MS 30U

typedef struct {
    bool candidate_pressed;
    bool stable_pressed;
    uint64_t candidate_since_ms;
    uint64_t last_update_ms;
} terminal_button_policy_t;

void terminal_button_policy_initialize(
    terminal_button_policy_t *policy,
    uint64_t now_ms,
    bool pressed);

/* Returns true only when a new debounced state is committed. */
bool terminal_button_policy_update(
    terminal_button_policy_t *policy,
    uint64_t now_ms,
    bool pressed,
    bool *stable_pressed);
