/*
 * WebSocket 客户端：连接远端 Agent，处理信令与音频帧收发。
 *
 * 事件 → protocol_handle_frame()（协议层）
 * 音频 → audio_play()（播放通路）
 * 状态 → ui_set_state()（显示）
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_websocket_client.h"

#include "ws_client.h"
#include "protocol.h"
#include "audio.h"
#include "ui.h"

static const char *TAG = "ws_client";

static esp_websocket_client_handle_t s_client = NULL;
static SemaphoreHandle_t s_send_mutex = NULL;
static volatile bool s_connected = false;

bool ws_is_connected(void)
{
    return s_connected;
}

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to agent");
        s_connected = true;
        ui_set_state(UI_STATE_CONNECTED);
        protocol_send_hello(s_client);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from agent");
        s_connected = false;
        audio_set_listening(false);
        ui_set_state(UI_STATE_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_DATA:
        protocol_handle_frame(s_client, data);
        break;

    default:
        break;
    }
}

void ws_client_init(void)
{
    s_send_mutex = xSemaphoreCreateMutex();

    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_AGENT_WS_URI,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .task_stack = 8192,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "failed to init websocket client");
        return;
    }
    esp_websocket_client_register_event(s_client, ESP_WEBSOCKET_EVENT_ANY,
                                        ws_event_handler, NULL);
    esp_websocket_client_start(s_client);
}

int ws_send_text(const char *text)
{
    if (!s_client || !s_connected) {
        return -1;
    }
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int r = esp_websocket_client_send_text(s_client, text, strlen(text),
                                           pdMS_TO_TICKS(1000));
    xSemaphoreGive(s_send_mutex);
    return r;
}

int ws_send_bin(const uint8_t *data, size_t len)
{
    if (!s_client || !s_connected) {
        return -1;
    }
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int r = esp_websocket_client_send_bin(s_client, (const char *)data, (int)len,
                                          pdMS_TO_TICKS(1000));
    xSemaphoreGive(s_send_mutex);
    return r;
}
