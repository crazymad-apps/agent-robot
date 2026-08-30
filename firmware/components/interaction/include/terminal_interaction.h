#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terminal_interaction terminal_interaction_t;

typedef enum {
    TERMINAL_INTERACTION_RESULT_OK = 0,
    TERMINAL_INTERACTION_RESULT_INVALID_ARGUMENT,
    TERMINAL_INTERACTION_RESULT_NO_MEMORY,
    TERMINAL_INTERACTION_RESULT_INVALID_STATE,
    TERMINAL_INTERACTION_RESULT_HARDWARE_FAILURE,
} terminal_interaction_result_t;

typedef enum {
    TERMINAL_INTERACTION_BOOT_ACTION_NONE = 0,
    TERMINAL_INTERACTION_BOOT_ACTION_CLEAR_PAIRING,
} terminal_interaction_boot_action_t;

typedef enum {
    TERMINAL_INTERACTION_BOOT_MILESTONE_PAIRING_RESET_ARMED = 0,
} terminal_interaction_boot_milestone_t;

/* The callback runs synchronously and must not block. */
typedef void (*terminal_interaction_boot_callback_t)(
    terminal_interaction_boot_milestone_t milestone,
    void *context);

typedef enum {
    TERMINAL_INTERACTION_EVENT_PTT_PRESSED = 0,
    TERMINAL_INTERACTION_EVENT_PTT_RELEASED,
} terminal_interaction_event_type_t;

typedef struct {
    terminal_interaction_event_type_t type;
    uint64_t occurred_at_ms;
} terminal_interaction_event_t;

/* The callback runs synchronously on the Button task and must not block. */
typedef void (*terminal_interaction_event_callback_t)(
    const terminal_interaction_event_t *event,
    void *context);

terminal_interaction_result_t terminal_interaction_create(
    terminal_interaction_t **interaction);

/*
 * Evaluates the power-on button gesture once. A debounced release before the
 * hold threshold returns NONE. After the threshold, CLEAR_PAIRING is returned
 * only after a debounced release, so a stuck-low input cannot erase identity.
 */
terminal_interaction_result_t terminal_interaction_evaluate_boot_action(
    terminal_interaction_t *interaction,
    terminal_interaction_boot_callback_t callback,
    void *callback_context,
    terminal_interaction_boot_action_t *action);

terminal_interaction_result_t terminal_interaction_start(
    terminal_interaction_t *interaction,
    terminal_interaction_event_callback_t callback,
    void *callback_context);

void terminal_interaction_stop(terminal_interaction_t *interaction);

void terminal_interaction_destroy(terminal_interaction_t *interaction);

#ifdef __cplusplus
}
#endif
