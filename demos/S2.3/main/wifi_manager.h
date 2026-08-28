#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_MANAGER_UNCONFIGURED = 0,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_DISCONNECTED,
    WIFI_MANAGER_ERROR,
} wifi_manager_state_t;

typedef struct {
    wifi_manager_state_t state;
    bool has_credentials;
    uint32_t reconnect_count;
    uint16_t last_disconnect_reason;
    int8_t rssi;
    char ip[16];
} wifi_manager_status_t;

esp_err_t wifi_manager_init(void);
void wifi_manager_get_status(wifi_manager_status_t *status);
