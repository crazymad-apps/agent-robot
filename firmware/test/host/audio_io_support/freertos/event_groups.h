#pragma once
#include "FreeRTOS.h"
typedef uint32_t *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
uint32_t xEventGroupClearBits(EventGroupHandle_t group, uint32_t bits);
uint32_t xEventGroupSetBits(EventGroupHandle_t group, uint32_t bits);
uint32_t xEventGroupWaitBits(EventGroupHandle_t group, uint32_t bits,
                           BaseType_t clear, BaseType_t all, TickType_t ticks);
void vEventGroupDelete(EventGroupHandle_t group);
