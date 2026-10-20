#pragma once
#include "FreeRTOS.h"
typedef struct audio_test_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock);
void vSemaphoreDelete(SemaphoreHandle_t lock);
