#include "voice_demo_client.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define CONNECTED_BIT BIT0
#define HELLO_ACK_BIT BIT1
#define PLAYBACK_DONE_BIT BIT2
#define ROUNDTRIP_ERROR_BIT BIT3

#define PROTOCOL_MAJOR 0
#define PROTOCOL_MINOR 1
#define PCM_BYTES_PER_FRAME 640
#define CONTROL_MESSAGE_BYTES 256
#define WEBSOCKET_BUFFER_BYTES 2048
#define WEBSOCKET_TASK_STACK_BYTES 6144
#define SEND_TIMEOUT_MS 3000
#define HELLO_TIMEOUT_MS 3000

typedef struct {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t events;
    SemaphoreHandle_t lock;
    uint8_t *playback_buffer;
    size_t playback_capacity;
    size_t playback_bytes;
    bool playback_active;
    bool initialized;
    uint32_t utterance_sequence;
} voice_demo_context_t;

static const char *TAG = "s3_websocket";
static voice_demo_context_t context;

static void set_roundtrip_error(const char *code)
{
    ESP_LOGE(TAG, "Protocol error: %s", code);
    xEventGroupSetBits(context.events, ROUNDTRIP_ERROR_BIT);
}

static const char *json_string(const cJSON *root, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static void handle_text_message(const char *data, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(data, length);
    if (root == NULL) {
        set_roundtrip_error("invalid_json");
        return;
    }

    const char *type = json_string(root, "type");
    if (type == NULL) {
        cJSON_Delete(root);
        set_roundtrip_error("missing_message_type");
        return;
    }

    if (strcmp(type, "hello_ack") == 0) {
        xEventGroupSetBits(context.events, HELLO_ACK_BIT);
    } else if (strcmp(type, "playback_start") == 0) {
        xSemaphoreTake(context.lock, portMAX_DELAY);
        context.playback_bytes = 0;
        context.playback_active = true;
        xSemaphoreGive(context.lock);
    } else if (strcmp(type, "playback_end") == 0) {
        xSemaphoreTake(context.lock, portMAX_DELAY);
        context.playback_active = false;
        xSemaphoreGive(context.lock);
        xEventGroupSetBits(context.events, PLAYBACK_DONE_BIT);
    } else if (strcmp(type, "error") == 0) {
        const char *code = json_string(root, "code");
        set_roundtrip_error(code == NULL ? "remote_error" : code);
    }

    cJSON_Delete(root);
}

static void handle_binary_message(const char *data, size_t length)
{
    xSemaphoreTake(context.lock, portMAX_DELAY);
    if (!context.playback_active) {
        xSemaphoreGive(context.lock);
        set_roundtrip_error("pcm_without_playback_start");
        return;
    }
    if (context.playback_bytes > context.playback_capacity ||
        length > context.playback_capacity - context.playback_bytes) {
        context.playback_active = false;
        xSemaphoreGive(context.lock);
        set_roundtrip_error("playback_buffer_overflow");
        return;
    }
    memcpy(context.playback_buffer + context.playback_bytes, data, length);
    context.playback_bytes += length;
    xSemaphoreGive(context.lock);
}

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_args;
    (void)event_base;
    esp_websocket_event_data_t *data = event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        xEventGroupClearBits(context.events,
                             HELLO_ACK_BIT | PLAYBACK_DONE_BIT | ROUNDTRIP_ERROR_BIT);
        xEventGroupSetBits(context.events, CONNECTED_BIT);
        ESP_LOGI(TAG, "Connected; authentication disabled");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        xEventGroupClearBits(context.events, CONNECTED_BIT | HELLO_ACK_BIT);
        xEventGroupSetBits(context.events, ROUNDTRIP_ERROR_BIT);
        ESP_LOGW(TAG, "Disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        xEventGroupSetBits(context.events, ROUNDTRIP_ERROR_BIT);
        ESP_LOGW(TAG, "Transport error");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
            break;
        }
        if (data->op_code == 0x1) {
            if (data->payload_offset != 0 || data->data_len != data->payload_len) {
                set_roundtrip_error("fragmented_control_message");
            } else {
                handle_text_message(data->data_ptr, (size_t)data->data_len);
            }
        } else if (data->op_code == 0x2) {
            handle_binary_message(data->data_ptr, (size_t)data->data_len);
        }
        break;
    default:
        break;
    }
}

static esp_err_t send_text(const char *message)
{
    const size_t length = strlen(message);
    const int sent = esp_websocket_client_send_text(
        context.client, message, (int)length, pdMS_TO_TICKS(SEND_TIMEOUT_MS));
    return sent == (int)length ? ESP_OK : ESP_FAIL;
}

static esp_err_t ensure_hello(void)
{
    if ((xEventGroupGetBits(context.events) & HELLO_ACK_BIT) != 0) {
        return ESP_OK;
    }

    char message[CONTROL_MESSAGE_BYTES];
    const int length = snprintf(
        message,
        sizeof(message),
        "{\"type\":\"hello\",\"protocol_major\":%d,\"protocol_minor\":%d,"
        "\"device_id\":\"esp32-s3-demo\"}",
        PROTOCOL_MAJOR,
        PROTOCOL_MINOR);
    if (length < 0 || (size_t)length >= sizeof(message)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t result = send_text(message);
    if (result != ESP_OK) {
        return result;
    }

    const EventBits_t bits = xEventGroupWaitBits(context.events,
                                                 HELLO_ACK_BIT | ROUNDTRIP_ERROR_BIT,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(HELLO_TIMEOUT_MS));
    return (bits & HELLO_ACK_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t voice_demo_client_init(const char *uri,
                                 uint8_t *playback_buffer,
                                 size_t playback_capacity)
{
    if (context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uri == NULL || uri[0] == '\0' || playback_buffer == NULL || playback_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    context.events = xEventGroupCreate();
    context.lock = xSemaphoreCreateMutex();
    if (context.events == NULL || context.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    context.playback_buffer = playback_buffer;
    context.playback_capacity = playback_capacity;

    const esp_websocket_client_config_t config = {
        .uri = uri,
        .disable_auto_reconnect = false,
        .task_stack = WEBSOCKET_TASK_STACK_BYTES,
        .buffer_size = WEBSOCKET_BUFFER_BYTES,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 5000,
        .ping_interval_sec = 10,
    };
    context.client = esp_websocket_client_init(&config);
    if (context.client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_websocket_register_events(
        context.client, WEBSOCKET_EVENT_ANY, websocket_event_handler, &context);
    if (result == ESP_OK) {
        result = esp_websocket_client_start(context.client);
    }
    if (result != ESP_OK) {
        (void)esp_websocket_client_destroy(context.client);
        context.client = NULL;
        return result;
    }

    context.initialized = true;
    ESP_LOGI(TAG, "Client started: uri=%s", uri);
    return ESP_OK;
}

bool voice_demo_client_is_connected(void)
{
    return context.initialized &&
           (xEventGroupGetBits(context.events) & CONNECTED_BIT) != 0 &&
           esp_websocket_client_is_connected(context.client);
}

esp_err_t voice_demo_client_roundtrip(const int16_t *capture_samples,
                                      size_t capture_sample_count,
                                      size_t *playback_sample_count,
                                      TickType_t response_timeout)
{
    if (!context.initialized || capture_samples == NULL || capture_sample_count == 0 ||
        playback_sample_count == NULL || !voice_demo_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(context.events, PLAYBACK_DONE_BIT | ROUNDTRIP_ERROR_BIT);
    esp_err_t result = ensure_hello();
    if (result != ESP_OK) {
        return result;
    }

    const uint32_t sequence = ++context.utterance_sequence;
    char utterance_id[48];
    int length = snprintf(utterance_id,
                          sizeof(utterance_id),
                          "u-%" PRIu32 "-%" PRId64,
                          sequence,
                          esp_timer_get_time());
    if (length < 0 || (size_t)length >= sizeof(utterance_id)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char message[CONTROL_MESSAGE_BYTES];
    length = snprintf(message,
                      sizeof(message),
                      "{\"type\":\"listen_start\",\"utterance_id\":\"%s\"}",
                      utterance_id);
    if (length < 0 || (size_t)length >= sizeof(message) ||
        (result = send_text(message)) != ESP_OK) {
        return length < 0 || (size_t)length >= sizeof(message) ? ESP_ERR_INVALID_SIZE : result;
    }

    const uint8_t *bytes = (const uint8_t *)capture_samples;
    const size_t byte_count = capture_sample_count * sizeof(capture_samples[0]);
    for (size_t offset = 0; offset < byte_count; offset += PCM_BYTES_PER_FRAME) {
        const size_t remaining = byte_count - offset;
        const size_t chunk = remaining < PCM_BYTES_PER_FRAME ? remaining : PCM_BYTES_PER_FRAME;
        const int sent = esp_websocket_client_send_bin(
            context.client,
            (const char *)(bytes + offset),
            (int)chunk,
            pdMS_TO_TICKS(SEND_TIMEOUT_MS));
        if (sent != (int)chunk) {
            return ESP_FAIL;
        }
    }

    length = snprintf(message,
                      sizeof(message),
                      "{\"type\":\"listen_stop\",\"utterance_id\":\"%s\"}",
                      utterance_id);
    if (length < 0 || (size_t)length >= sizeof(message) ||
        (result = send_text(message)) != ESP_OK) {
        return length < 0 || (size_t)length >= sizeof(message) ? ESP_ERR_INVALID_SIZE : result;
    }

    const EventBits_t bits = xEventGroupWaitBits(context.events,
                                                 PLAYBACK_DONE_BIT | ROUNDTRIP_ERROR_BIT,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 response_timeout);
    if ((bits & PLAYBACK_DONE_BIT) == 0) {
        return (bits & ROUNDTRIP_ERROR_BIT) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(context.lock, portMAX_DELAY);
    const size_t playback_bytes = context.playback_bytes;
    xSemaphoreGive(context.lock);
    if (playback_bytes == 0 || playback_bytes % sizeof(int16_t) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    *playback_sample_count = playback_bytes / sizeof(int16_t);
    ESP_LOGI(TAG,
             "Roundtrip complete: utterance=%s tx=%u rx=%u bytes",
             utterance_id,
             (unsigned int)byte_count,
             (unsigned int)playback_bytes);
    return ESP_OK;
}
