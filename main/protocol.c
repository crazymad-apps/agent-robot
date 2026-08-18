/*
 * 协议适配层：远端 Agent 消息的解析与封装。
 *
 * 约定（草稿 v0.1，见 README）：
 *   - 文本帧: JSON 信令，{"type": "...", ...}
 *   - 二进制帧: 音频数据（16kHz 16bit 单声道 PCM）
 *
 * 远端 Agent 协议如有差异，改动仅限本文件。
 */
#include <string.h>

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

#include "protocol.h"
#include "ws_client.h"
#include "audio.h"
#include "ui.h"

static const char *TAG = "protocol";

/* WebSocket opcodes（RFC 6455） */
#define WS_OPCODE_TEXT 0x1
#define WS_OPCODE_BIN  0x2

void protocol_send_hello(esp_websocket_client_handle_t client)
{
    (void)client;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device", CONFIG_AGENT_DEVICE_NAME);
    cJSON_AddStringToObject(root, "hw", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(root, "sample_rate", AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "channels", 1);

    char *s = cJSON_PrintUnformatted(root);
    if (s != NULL) {
        ws_send_text(s);
        ESP_LOGI(TAG, "hello sent: %s", s);
        cJSON_free(s);
    }
    cJSON_Delete(root);
}

static void handle_text_frame(const char *payload, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid json: %.*s", (int)len, payload);
        return;
    }

    const cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (type_item == NULL || !cJSON_IsString(type_item)) {
        ESP_LOGW(TAG, "frame without type field");
        cJSON_Delete(root);
        return;
    }
    const char *type = type_item->valuestring;

    if (strcmp(type, "state") == 0) {
        const cJSON *st = cJSON_GetObjectItem(root, "state");
        if (st != NULL && cJSON_IsString(st)) {
            ui_set_state(ui_state_from_string(st->valuestring));
        }
    } else if (strcmp(type, "text") == 0) {
        const cJSON *txt = cJSON_GetObjectItem(root, "text");
        if (txt != NULL && cJSON_IsString(txt)) {
            ui_show_text(txt->valuestring);
        }
    } else if (strcmp(type, "audio_start") == 0) {
        audio_play_start();
    } else if (strcmp(type, "audio_end") == 0) {
        audio_play_end();
    } else {
        ESP_LOGI(TAG, "unhandled frame type: %s", type);
    }

    cJSON_Delete(root);
}

void protocol_handle_frame(esp_websocket_client_handle_t client,
                           const esp_websocket_event_data_t *data)
{
    (void)client;

    if (data->op_code == WS_OPCODE_BIN) {
        audio_play((const uint8_t *)data->data_ptr, data->data_len);
        return;
    }
    if (data->op_code == WS_OPCODE_TEXT) {
        handle_text_frame(data->data_ptr, data->data_len);
    }
}
