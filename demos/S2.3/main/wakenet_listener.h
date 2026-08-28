#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    int sample_rate_hz;
    int feed_samples;
    int feed_channels;
    unsigned long wake_count;
} wakenet_listener_status_t;

esp_err_t wakenet_listener_init(void);
esp_err_t wakenet_listener_poll(bool *detected);
esp_err_t wakenet_listener_pause(void);
esp_err_t wakenet_listener_resume(void);
void wakenet_listener_get_status(wakenet_listener_status_t *status);
