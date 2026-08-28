#include "device_control_envelope.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void verify_fixture(const char *encoded)
{
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(encoded, strlen(encoded), &envelope)
           == DEVICE_PROTOCOL_RESULT_OK);
    assert(envelope != NULL);
    assert(device_control_envelope_protocol_minor(envelope) == 0U);
    assert(strcmp(device_control_envelope_message_id(envelope), "message-error-fixture") == 0);
    assert(strcmp(device_control_envelope_message_type(envelope), "error") == 0);

    const cJSON *payload = device_control_envelope_payload(envelope);
    assert(cJSON_IsObject(payload));
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(payload, "code");
    const cJSON *correlation = cJSON_GetObjectItemCaseSensitive(payload, "correlation_message_id");
    const cJSON *recoverable = cJSON_GetObjectItemCaseSensitive(payload, "recoverable");
    assert(cJSON_IsString(code) && strcmp(code->valuestring, "authentication_failed") == 0);
    assert(cJSON_IsString(correlation)
           && strcmp(correlation->valuestring, "message-hello-fixture") == 0);
    assert(cJSON_IsFalse(recoverable));

    char round_trip[1024] = {0};
    size_t round_trip_size = 0U;
    assert(device_control_envelope_encode(
               device_control_envelope_message_id(envelope),
               device_control_envelope_message_type(envelope),
               payload,
               round_trip,
               sizeof(round_trip),
               &round_trip_size)
           == DEVICE_PROTOCOL_RESULT_OK);
    assert(round_trip_size == strlen(encoded));
    assert(memcmp(round_trip, encoded, round_trip_size) == 0);

    size_t required_size = 0U;
    assert(device_control_envelope_encode(
               device_control_envelope_message_id(envelope),
               device_control_envelope_message_type(envelope),
               payload,
               round_trip,
               1U,
               &required_size)
           == DEVICE_PROTOCOL_RESULT_OUTPUT_TOO_SMALL);
    assert(required_size == round_trip_size);
    device_control_envelope_release(envelope);
}

static void verify_rejected(const char *encoded, device_protocol_result_t expected)
{
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(encoded, strlen(encoded), &envelope) == expected);
    assert(envelope == NULL);
}

int main(int argument_count, char **arguments)
{
    assert(argument_count == 2);
    verify_fixture(arguments[1]);

    verify_rejected(
        "{\"protocol_major\":2,\"protocol_minor\":0,\"message_id\":\"m\",\"type\":\"ping\",\"payload\":{}}",
        DEVICE_PROTOCOL_RESULT_UNSUPPORTED_PROTOCOL);
    verify_rejected(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\",\"type\":\"ping\",\"payload\":{},\"extra\":true}",
        DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    verify_rejected(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\",\"message_id\":\"n\",\"type\":\"ping\",\"payload\":{}}",
        DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    verify_rejected(
        "{\"protocol_major\":1,\"protocol_minor\":1.5,\"message_id\":\"m\",\"type\":\"ping\",\"payload\":{}}",
        DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    verify_rejected(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\" \",\"type\":\"ping\",\"payload\":{}}",
        DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    verify_rejected(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\",\"type\":\"ping\",\"payload\":{}} trailing",
        DEVICE_PROTOCOL_RESULT_INVALID_JSON);
    verify_rejected("[]", DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);

    const char *null_payload =
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\",\"type\":\"event\",\"payload\":null} \n";
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(null_payload, strlen(null_payload), &envelope)
           == DEVICE_PROTOCOL_RESULT_OK);
    assert(cJSON_IsNull(device_control_envelope_payload(envelope)));
    device_control_envelope_release(envelope);

    char oversized[DEVICE_CONTROL_MESSAGE_MAX_BYTES + 1U] = {0};
    assert(device_control_envelope_decode(oversized, sizeof(oversized), &envelope)
           == DEVICE_PROTOCOL_RESULT_MESSAGE_TOO_LARGE);

    char long_message_id[DEVICE_MESSAGE_ID_MAX_BYTES + 2U];
    memset(long_message_id, 'a', sizeof(long_message_id) - 1U);
    long_message_id[sizeof(long_message_id) - 1U] = '\0';
    cJSON *payload = cJSON_CreateObject();
    assert(payload != NULL);
    char encoded[256] = {0};
    size_t encoded_size = 0U;
    assert(device_control_envelope_encode(
               long_message_id,
               "ping",
               payload,
               encoded,
               sizeof(encoded),
               &encoded_size)
           == DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);
    cJSON_Delete(payload);

    puts("device control envelope fixture and strict schema checks passed");
    return 0;
}
