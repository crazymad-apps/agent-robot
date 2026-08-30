#pragma once

#include <stdbool.h>

#include "device_pairing.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terminal_ui terminal_ui_t;

typedef enum {
    TERMINAL_UI_RESULT_OK = 0,
    TERMINAL_UI_RESULT_INVALID_ARGUMENT,
    TERMINAL_UI_RESULT_INVALID_STATE,
    TERMINAL_UI_RESULT_NO_MEMORY,
    TERMINAL_UI_RESULT_HARDWARE_FAILURE,
} terminal_ui_result_t;

typedef enum {
    TERMINAL_UI_STATE_STARTING = 0,
    TERMINAL_UI_STATE_CONNECTING,
    TERMINAL_UI_STATE_WAITING_FOR_PAIRING,
    TERMINAL_UI_STATE_PAIRING,
    TERMINAL_UI_STATE_READY,
    TERMINAL_UI_STATE_LISTENING,
    TERMINAL_UI_STATE_THINKING,
    TERMINAL_UI_STATE_SPEAKING,
    TERMINAL_UI_STATE_CHECK_DESKTOP,
    TERMINAL_UI_STATE_ERROR,
} terminal_ui_state_t;

typedef enum {
    TERMINAL_UI_ERROR_NONE = 0,
    TERMINAL_UI_ERROR_MULTIPLE_HOSTS,
    TERMINAL_UI_ERROR_CERTIFICATE,
    TERMINAL_UI_ERROR_IDENTITY,
    TERMINAL_UI_ERROR_PROTOCOL,
    TERMINAL_UI_ERROR_NETWORK,
} terminal_ui_error_t;

typedef struct {
    terminal_ui_state_t state;
    terminal_ui_error_t error;
    bool pairing_code_visible;
    char pairing_code[DEVICE_PAIRING_CODE_CAPACITY];
} terminal_ui_projection_t;

terminal_ui_result_t terminal_ui_create(terminal_ui_t **ui);

terminal_ui_result_t terminal_ui_start(terminal_ui_t *ui);

terminal_ui_result_t terminal_ui_update(
    terminal_ui_t *ui,
    const terminal_ui_projection_t *projection);

void terminal_ui_stop(terminal_ui_t *ui);

void terminal_ui_destroy(terminal_ui_t *ui);

#ifdef __cplusplus
}
#endif
