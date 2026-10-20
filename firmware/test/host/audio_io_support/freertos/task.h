#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *context, unsigned int priority, TaskHandle_t *handle);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
void xTaskNotifyGive(TaskHandle_t task);
void vTaskDelete(TaskHandle_t task);
