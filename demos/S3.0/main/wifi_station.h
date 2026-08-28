#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t wifi_station_init(void);
esp_err_t wifi_station_wait_connected(TickType_t timeout);
