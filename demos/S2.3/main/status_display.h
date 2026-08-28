#pragma once

#include "esp_err.h"

typedef enum {
    ROBOT_STATUS_IDLE = 0,
    ROBOT_STATUS_LISTENING,
    ROBOT_STATUS_PLAYING,
    ROBOT_STATUS_ERROR,
} robot_status_t;

typedef enum {
    ROBOT_NETWORK_SETUP = 0,
    ROBOT_NETWORK_CONNECTING,
    ROBOT_NETWORK_CONNECTED,
    ROBOT_NETWORK_DISCONNECTED,
    ROBOT_NETWORK_ERROR,
} robot_network_status_t;

esp_err_t status_display_init(void);
void status_display_set(robot_status_t status);
void status_display_set_network(robot_network_status_t status);
