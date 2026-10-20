#pragma once

#include <stdint.h>

/* Single-task deterministic scheduler used only by the host audio worker test. */
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10U))
#define BIT0 1U
