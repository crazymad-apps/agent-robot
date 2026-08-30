#include "device_application_heartbeat.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static device_protocol_result_t decode_ping(const char *encoded,
                                            device_application_heartbeat_t *heartbeat)
{
    device_control_envelope_t *envelope = NULL;
    const device_protocol_result_t envelope_result =
        device_control_envelope_decode(encoded, strlen(encoded), &envelope);
    if (envelope_result != DEVICE_PROTOCOL_RESULT_OK) {
        return envelope_result;
    }
    const device_protocol_result_t heartbeat_result =
        device_application_heartbeat_decode_ping(envelope, heartbeat);
    device_control_envelope_release(envelope);
    return heartbeat_result;
}

int main(void)
{
    const char *ping = "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"ping-1\","
                       "\"type\":\"ping\",\"payload\":{\"nonce\":\"heartbeat-nonce\","
                       "\"sent_at_ms\":1788068000000}}";
    device_application_heartbeat_t heartbeat = {0};
    assert(decode_ping(ping, &heartbeat) == DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(heartbeat.nonce, "heartbeat-nonce") == 0);
    assert(heartbeat.sent_at_ms == INT64_C(1788068000000));

    char pong[512] = {0};
    size_t pong_size = 0U;
    assert(device_application_heartbeat_encode_pong("pong-1", &heartbeat, pong, sizeof(pong),
                                                    &pong_size) == DEVICE_PROTOCOL_RESULT_OK);
    device_control_envelope_t *pong_envelope = NULL;
    assert(device_control_envelope_decode(pong, pong_size, &pong_envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(device_control_envelope_message_type(pong_envelope), "pong") == 0);
    const cJSON *pong_payload = device_control_envelope_payload(pong_envelope);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(pong_payload, "nonce")->valuestring,
                  "heartbeat-nonce") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(pong_payload, "sent_at_ms")->valuedouble ==
           1788068000000.0);
    device_control_envelope_release(pong_envelope);

    assert(decode_ping("{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\","
                       "\"type\":\"ping\",\"payload\":{\"nonce\":\"n\"}}",
                       &heartbeat) == DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    assert(decode_ping("{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\","
                       "\"type\":\"ping\",\"payload\":{\"nonce\":\"n\",\"sent_at_ms\":1,"
                       "\"extra\":true}}",
                       &heartbeat) == DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    assert(decode_ping("{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\","
                       "\"type\":\"ping\",\"payload\":{\"nonce\":\"bad nonce\","
                       "\"sent_at_ms\":1}}",
                       &heartbeat) == DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);
    assert(decode_ping("{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m\","
                       "\"type\":\"ping\",\"payload\":{\"nonce\":\"n\","
                       "\"sent_at_ms\":1.5}}",
                       &heartbeat) == DEVICE_PROTOCOL_RESULT_INVALID_ENVELOPE);

    puts("application heartbeat ping/pong schema checks passed");
    return 0;
}
