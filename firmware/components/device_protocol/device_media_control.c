#include "device_media_control.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "device_control_envelope.h"

static bool identifier_is_valid(const char *value, size_t maximum_size)
{
    if (value == NULL) {
        return false;
    }
    const size_t value_size = strlen(value);
    if (value_size == 0U || value_size > maximum_size) {
        return false;
    }
    for (size_t index = 0U; index < value_size; ++index) {
        if (!isspace((unsigned char)value[index])) {
            return true;
        }
    }
    return false;
}

static const char *output_preference_name(device_output_preference_t output_preference)
{
    switch (output_preference) {
    case DEVICE_OUTPUT_PREFERENCE_TEXT:
        return "text";
    case DEVICE_OUTPUT_PREFERENCE_AUDIO:
        return "audio";
    case DEVICE_OUTPUT_PREFERENCE_TEXT_AND_AUDIO:
        return "text_and_audio";
    default:
        return NULL;
    }
}

static device_protocol_result_t encode(const char *message_id, const char *message_type,
                                       cJSON *payload, char *encoded, size_t encoded_capacity,
                                       size_t *encoded_size)
{
    if (payload == NULL) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    const device_protocol_result_t result = device_control_envelope_encode(
        message_id, message_type, payload, encoded, encoded_capacity, encoded_size);
    cJSON_Delete(payload);
    return result;
}

device_protocol_result_t device_media_control_encode_listen_start(
    const char *message_id, const char *client_input_id, uint32_t stream_id,
    device_output_preference_t output_preference, char *encoded, size_t encoded_capacity,
    size_t *encoded_size)
{
    const char *preference = output_preference_name(output_preference);
    if (!identifier_is_valid(client_input_id, DEVICE_MEDIA_CLIENT_INPUT_ID_MAX_BYTES) ||
        stream_id == 0U || preference == NULL) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON *format = cJSON_CreateObject();
    if (payload == NULL || format == NULL ||
        cJSON_AddStringToObject(payload, "client_input_id", client_input_id) == NULL ||
        cJSON_AddNumberToObject(payload, "stream_id", stream_id) == NULL ||
        cJSON_AddStringToObject(format, "encoding", "pcm_s16le") == NULL ||
        cJSON_AddNumberToObject(format, "sample_rate_hz", 16000U) == NULL ||
        cJSON_AddNumberToObject(format, "channels", 1U) == NULL ||
        cJSON_AddNumberToObject(format, "frame_duration_ms", 20U) == NULL) {
        cJSON_Delete(format);
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    if (!cJSON_AddItemToObject(payload, "format", format)) {
        cJSON_Delete(format);
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    format = NULL;
    if (cJSON_AddStringToObject(payload, "output_preference", preference) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    return encode(message_id, "listen_start", payload, encoded, encoded_capacity, encoded_size);
}

device_protocol_result_t
device_media_control_encode_listen_stop(const char *message_id, uint32_t stream_id,
                                        uint32_t last_sequence, char *encoded,
                                        size_t encoded_capacity, size_t *encoded_size)
{
    if (stream_id == 0U) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL || cJSON_AddNumberToObject(payload, "stream_id", stream_id) == NULL ||
        cJSON_AddNumberToObject(payload, "last_sequence", last_sequence) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    return encode(message_id, "listen_stop", payload, encoded, encoded_capacity, encoded_size);
}

device_protocol_result_t
device_media_control_encode_listen_cancel(const char *message_id, uint32_t stream_id, char *encoded,
                                          size_t encoded_capacity, size_t *encoded_size)
{
    if (stream_id == 0U) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL || cJSON_AddNumberToObject(payload, "stream_id", stream_id) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    return encode(message_id, "listen_cancel", payload, encoded, encoded_capacity, encoded_size);
}

device_protocol_result_t
device_media_control_encode_playback_cancel(const char *message_id, const char *output_id,
                                            uint32_t stream_id, char *encoded,
                                            size_t encoded_capacity, size_t *encoded_size)
{
    if (!identifier_is_valid(output_id, DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES) || stream_id == 0U) {
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL || cJSON_AddStringToObject(payload, "output_id", output_id) == NULL ||
        cJSON_AddNumberToObject(payload, "stream_id", stream_id) == NULL) {
        cJSON_Delete(payload);
        return DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT;
    }
    return encode(message_id, "playback_cancel", payload, encoded, encoded_capacity, encoded_size);
}
