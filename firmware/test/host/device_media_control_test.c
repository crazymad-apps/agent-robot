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

static device_control_envelope_t *envelope_from_payload(const char *message_type,
                                                        const cJSON *payload)
{
    char encoded[4096] = {0};
    size_t encoded_size = 0U;
    assert(device_control_envelope_encode("server-message", message_type, payload, encoded,
                                          sizeof(encoded),
                                          &encoded_size) == DEVICE_PROTOCOL_RESULT_OK);
    return decode(encoded, encoded_size, message_type);
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

static void verify_input_accepted(void)
{
    cJSON *payload = cJSON_CreateObject();
    assert(payload != NULL);
    assert(cJSON_AddStringToObject(payload, "client_input_id", "client-input") != NULL);
    assert(cJSON_AddStringToObject(payload, "input_id", "input-id") != NULL);
    assert(cJSON_AddStringToObject(payload, "run_id", "run-id") != NULL);
    assert(cJSON_AddStringToObject(payload, "queue_state", "accepted") != NULL);
    device_control_envelope_t *envelope = envelope_from_payload("input_accepted", payload);
    device_media_input_accepted_t accepted = {0};
    assert(device_media_control_decode_input_accepted(envelope, &accepted) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(accepted.client_input_id, "client-input") == 0);
    assert(strcmp(accepted.input_id, "input-id") == 0);
    assert(strcmp(accepted.run_id, "run-id") == 0);
    assert(accepted.queue_state == DEVICE_MEDIA_RUN_STATUS_ACCEPTED);
    device_control_envelope_release(envelope);

    assert(cJSON_AddBoolToObject(payload, "unexpected", true) != NULL);
    envelope = envelope_from_payload("input_accepted", payload);
    assert(device_media_control_decode_input_accepted(envelope, &accepted) ==
           DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    device_control_envelope_release(envelope);
    cJSON_Delete(payload);
}

static void verify_input_segment_accepted(void)
{
    cJSON *payload = cJSON_CreateObject();
    assert(payload != NULL);
    assert(cJSON_AddStringToObject(payload, "client_input_id", "client-segment") != NULL);
    assert(cJSON_AddNumberToObject(payload, "stream_id", 17U) != NULL);
    device_control_envelope_t *envelope =
        envelope_from_payload("input_segment_accepted", payload);
    device_media_input_segment_accepted_t accepted = {0};
    assert(device_media_control_decode_input_segment_accepted(envelope, &accepted) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(accepted.client_input_id, "client-segment") == 0);
    assert(accepted.stream_id == 17U);
    device_control_envelope_release(envelope);

    assert(cJSON_ReplaceItemInObject(payload, "stream_id", cJSON_CreateNumber(0U)));
    envelope = envelope_from_payload("input_segment_accepted", payload);
    assert(device_media_control_decode_input_segment_accepted(envelope, &accepted) ==
           DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    device_control_envelope_release(envelope);
    cJSON_Delete(payload);
}

static void verify_state_changed(void)
{
    cJSON *payload = cJSON_CreateObject();
    assert(payload != NULL);
    assert(cJSON_AddNullToObject(payload, "run_id") != NULL);
    assert(cJSON_AddStringToObject(payload, "client_input_id", "client-input") != NULL);
    assert(cJSON_AddStringToObject(payload, "state", "recognizing") != NULL);
    assert(cJSON_AddNullToObject(payload, "reason") != NULL);
    device_control_envelope_t *envelope = envelope_from_payload("state_changed", payload);
    device_media_state_changed_t state_changed = {0};
    assert(device_media_control_decode_state_changed(envelope, &state_changed) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(!state_changed.has_run_id);
    assert(state_changed.has_client_input_id);
    assert(strcmp(state_changed.client_input_id, "client-input") == 0);
    assert(state_changed.state == DEVICE_MEDIA_INTERACTION_STATE_RECOGNIZING);
    assert(!state_changed.has_reason);
    device_control_envelope_release(envelope);

    cJSON_ReplaceItemInObjectCaseSensitive(payload, "state", cJSON_CreateString("unknown"));
    envelope = envelope_from_payload("state_changed", payload);
    assert(device_media_control_decode_state_changed(envelope, &state_changed) ==
           DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    device_control_envelope_release(envelope);
    cJSON_Delete(payload);
}

static cJSON *playback_start_payload(void)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *format = cJSON_CreateObject();
    assert(payload != NULL && format != NULL);
    assert(cJSON_AddStringToObject(payload, "output_id", "output-id") != NULL);
    assert(cJSON_AddStringToObject(payload, "run_id", "run-id") != NULL);
    assert(cJSON_AddNumberToObject(payload, "stream_id", 9U) != NULL);
    assert(cJSON_AddStringToObject(format, "encoding", "pcm_s16le") != NULL);
    assert(cJSON_AddNumberToObject(format, "sample_rate_hz", 16000U) != NULL);
    assert(cJSON_AddNumberToObject(format, "channels", 1U) != NULL);
    assert(cJSON_AddNumberToObject(format, "frame_duration_ms", 20U) != NULL);
    assert(cJSON_AddItemToObject(payload, "format", format));
    assert(cJSON_AddStringToObject(payload, "text", "spoken output") != NULL);
    assert(cJSON_AddNumberToObject(payload, "sample_count", 321U) != NULL);
    return payload;
}

static void verify_playback_events(void)
{
    cJSON *payload = playback_start_payload();
    device_control_envelope_t *envelope = envelope_from_payload("playback_start", payload);
    device_media_playback_start_t playback_start = {0};
    assert(device_media_control_decode_playback_start(envelope, &playback_start) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(playback_start.output_id, "output-id") == 0);
    assert(strcmp(playback_start.run_id, "run-id") == 0);
    assert(playback_start.stream_id == 9U);
    assert(playback_start.sample_count == 321U);
    device_control_envelope_release(envelope);

    cJSON_ReplaceItemInObjectCaseSensitive(payload, "sample_count", cJSON_CreateNumber(1.5));
    envelope = envelope_from_payload("playback_start", payload);
    assert(device_media_control_decode_playback_start(envelope, &playback_start) ==
           DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    device_control_envelope_release(envelope);
    cJSON_Delete(payload);

    payload = cJSON_CreateObject();
    assert(payload != NULL);
    assert(cJSON_AddStringToObject(payload, "output_id", "output-id") != NULL);
    assert(cJSON_AddNumberToObject(payload, "stream_id", 9U) != NULL);
    assert(cJSON_AddStringToObject(payload, "reason", "completed") != NULL);
    envelope = envelope_from_payload("playback_end", payload);
    device_media_playback_end_t playback_end = {0};
    assert(device_media_control_decode_playback_end(envelope, &playback_end) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(playback_end.output_id, "output-id") == 0);
    assert(playback_end.stream_id == 9U);
    assert(strcmp(playback_end.reason, "completed") == 0);
    device_control_envelope_release(envelope);
    cJSON_Delete(payload);
}

int main(void)
{
    verify_listen_start();
    verify_simple_controls();
    verify_input_segment_accepted();
    verify_input_accepted();
    verify_state_changed();
    verify_playback_events();
    puts("media control codec schema checks passed");
    return 0;
}
