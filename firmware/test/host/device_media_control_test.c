#include "device_media_control.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "device_control_envelope.h"

static device_control_envelope_t *decode(const char *encoded, size_t encoded_size,
                                         const char *expected_type)
{
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(encoded, encoded_size, &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(device_control_envelope_message_type(envelope), expected_type) == 0);
    return envelope;
}

static void verify_listen_start(void)
{
    char encoded[1024] = {0};
    size_t encoded_size = 0U;
    assert(device_media_control_encode_listen_start(
               "message-start", "client-input", 7U, DEVICE_OUTPUT_PREFERENCE_AUDIO, encoded,
               sizeof(encoded), &encoded_size) == DEVICE_PROTOCOL_RESULT_OK);
    device_control_envelope_t *envelope = decode(encoded, encoded_size, "listen_start");
    const cJSON *payload = device_control_envelope_payload(envelope);
    assert(cJSON_GetArraySize(payload) == 4);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(payload, "client_input_id")->valuestring,
                  "client-input") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(payload, "stream_id")->valuedouble == 7.0);
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(payload, "format");
    assert(cJSON_GetArraySize(format) == 4);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(format, "encoding")->valuestring, "pcm_s16le") ==
           0);
    assert(cJSON_GetObjectItemCaseSensitive(format, "sample_rate_hz")->valuedouble == 16000.0);
    assert(cJSON_GetObjectItemCaseSensitive(format, "channels")->valuedouble == 1.0);
    assert(cJSON_GetObjectItemCaseSensitive(format, "frame_duration_ms")->valuedouble == 20.0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(payload, "output_preference")->valuestring,
                  "audio") == 0);
    device_control_envelope_release(envelope);

    assert(device_media_control_encode_listen_start(
               "message-start", " ", 7U, DEVICE_OUTPUT_PREFERENCE_AUDIO, encoded, sizeof(encoded),
               &encoded_size) == DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);
    assert(device_media_control_encode_listen_start(
               "message-start", "client-input", 0U, DEVICE_OUTPUT_PREFERENCE_AUDIO, encoded,
               sizeof(encoded), &encoded_size) == DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);
}

static void verify_simple_controls(void)
{
    char encoded[512] = {0};
    size_t encoded_size = 0U;
    assert(device_media_control_encode_listen_stop("message-stop", 7U, 13U, encoded,
                                                   sizeof(encoded),
                                                   &encoded_size) == DEVICE_PROTOCOL_RESULT_OK);
    device_control_envelope_t *envelope = decode(encoded, encoded_size, "listen_stop");
    const cJSON *payload = device_control_envelope_payload(envelope);
    assert(cJSON_GetArraySize(payload) == 2);
    assert(cJSON_GetObjectItemCaseSensitive(payload, "stream_id")->valuedouble == 7.0);
    assert(cJSON_GetObjectItemCaseSensitive(payload, "last_sequence")->valuedouble == 13.0);
    device_control_envelope_release(envelope);

    assert(device_media_control_encode_listen_cancel("message-cancel", 7U, encoded, sizeof(encoded),
                                                     &encoded_size) == DEVICE_PROTOCOL_RESULT_OK);
    envelope = decode(encoded, encoded_size, "listen_cancel");
    payload = device_control_envelope_payload(envelope);
    assert(cJSON_GetArraySize(payload) == 1);
    assert(cJSON_GetObjectItemCaseSensitive(payload, "stream_id")->valuedouble == 7.0);
    device_control_envelope_release(envelope);

    assert(device_media_control_encode_playback_cancel("message-playback-cancel", "output-id", 9U,
                                                       encoded, sizeof(encoded),
                                                       &encoded_size) == DEVICE_PROTOCOL_RESULT_OK);
    envelope = decode(encoded, encoded_size, "playback_cancel");
    payload = device_control_envelope_payload(envelope);
    assert(cJSON_GetArraySize(payload) == 2);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(payload, "output_id")->valuestring,
                  "output-id") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(payload, "stream_id")->valuedouble == 9.0);
    device_control_envelope_release(envelope);

    assert(device_media_control_encode_listen_stop("message-stop", 0U, 0U, encoded, sizeof(encoded),
                                                   &encoded_size) ==
           DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);
    assert(device_media_control_encode_playback_cancel("message-playback-cancel", "", 9U, encoded,
                                                       sizeof(encoded), &encoded_size) ==
           DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);
}

int main(void)
{
    verify_listen_start();
    verify_simple_controls();
    puts("media control encoder schema checks passed");
    return 0;
}
