#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t voice_demo_client_init(const char *uri,
                                 uint8_t *playback_buffer,
                                 size_t playback_capacity);
bool voice_demo_client_is_connected(void);
esp_err_t voice_demo_client_roundtrip(const int16_t *capture_samples,
                                      size_t capture_sample_count,
                                      size_t *playback_sample_count,
                                      TickType_t response_timeout);
