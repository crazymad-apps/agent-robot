#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_control_envelope.h"
#include "device_pairing_protocol.h"

static const device_capabilities_t CAPABILITIES = {
    .input_text = false,
    .input_pcm16_16k_mono = true,
    .output_text = false,
    .output_pcm16_16k_mono = true,
    .playback_cancel = true,
    .display_status = true,
    .display_transcript = false,
};

static device_control_envelope_t *decode(const char *encoded)
{
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(encoded, strlen(encoded), &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(envelope != NULL);
    return envelope;
}

static void verify_client_messages(void)
{
    uint8_t share[DEVICE_SPAKE2_SHARE_BYTES];
    uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES];
    uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES];
    uint8_t mac[DEVICE_SPAKE2_BINDING_MAC_BYTES];
    for (size_t index = 0U; index < sizeof(share); ++index) share[index] = (uint8_t)index;
    for (size_t index = 0U; index < sizeof(public_key); ++index) {
        public_key[index] = (uint8_t)(0x20U + index);
        mac[index] = (uint8_t)(0x40U + index);
    }
    for (size_t index = 0U; index < sizeof(signature); ++index) {
        signature[index] = (uint8_t)(0x60U + index);
    }

    char encoded[2048];
    size_t encoded_size = 0U;
    assert(device_pairing_encode_hello(
               "hello-message",
               "pairing-request",
               "Agent Robot Terminal",
               "device-nonce",
               &CAPABILITIES,
               share,
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_PAIRING_PROTOCOL_RESULT_OK);
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(encoded, encoded_size, &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(device_control_envelope_message_type(envelope), "pairing_hello") == 0);
    const cJSON *payload = device_control_envelope_payload(envelope);
    assert(strcmp(
               cJSON_GetObjectItemCaseSensitive(payload, "pairing_request_id")->valuestring,
               "pairing-request") == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(payload, "capabilities"),
        "input_pcm16_16k_mono")));
    device_control_envelope_release(envelope);

    assert(device_pairing_encode_confirmation(
               "confirm-message",
               "pairing-request",
               mac,
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_PAIRING_PROTOCOL_RESULT_OK);
    envelope = NULL;
    assert(device_control_envelope_decode(encoded, encoded_size, &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(device_control_envelope_message_type(envelope), "pairing_confirm") == 0);
    device_control_envelope_release(envelope);

    assert(device_pairing_encode_bind(
               "bind-message",
               "pairing-request",
               public_key,
               signature,
               mac,
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_PAIRING_PROTOCOL_RESULT_OK);
    envelope = NULL;
    assert(device_control_envelope_decode(encoded, encoded_size, &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(device_control_envelope_message_type(envelope), "pairing_bind") == 0);
    device_control_envelope_release(envelope);

    assert(device_pairing_encode_commit(
               "commit-message",
               "pairing-request",
               "device-paired",
               signature,
               mac,
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_PAIRING_PROTOCOL_RESULT_OK);
    envelope = NULL;
    assert(device_control_envelope_decode(encoded, encoded_size, &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(device_control_envelope_message_type(envelope), "pairing_commit") == 0);
    device_control_envelope_release(envelope);

    assert(device_pairing_encode_hello(
               "message",
               "pairing-request",
               "Agent Robot Terminal",
               "device-nonce",
               &CAPABILITIES,
               share,
               encoded,
               16U,
               &encoded_size) == DEVICE_PAIRING_PROTOCOL_RESULT_OUTPUT_TOO_SMALL);
}

static void verify_host_messages(void)
{
    device_control_envelope_t *envelope = decode(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"pending\"," 
        "\"type\":\"pairing_pending\",\"payload\":{\"pairing_request_id\":"
        "\"pairing-request\",\"expires_at_ms\":1787790300000}}");
    device_pairing_pending_t pending = {0};
    assert(device_pairing_decode_pending(envelope, "pairing-request", &pending) ==
           DEVICE_PAIRING_PROTOCOL_RESULT_OK);
    assert(pending.expires_at_ms == 1787790300000LL);
    device_control_envelope_release(envelope);

    envelope = decode(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"pake\"," 
        "\"type\":\"pairing_pake\",\"payload\":{\"pairing_request_id\":"
        "\"pairing-request\",\"host_nonce\":\"host-nonce\",\"pake_share\":"
        "\"BJOVw9GEr8DMp481poxTdlcGp34O0wULhrujTilqHd1kuS2zRLKHpybBW3JytcWHGDfIVjI8fT1iHXEBga5NQOY\"," 
        "\"confirmation_mac\":\"4lS4w6y0TnLOX0jJZqGZZ4ZJj06bRcI6eNMcO4p2grU\"}}");
    device_pairing_pake_t pake = {0};
    assert(device_pairing_decode_pake(envelope, "pairing-request", &pake) ==
           DEVICE_PAIRING_PROTOCOL_RESULT_OK);
    assert(strcmp(pake.host_nonce, "host-nonce") == 0);
    device_control_envelope_release(envelope);

    envelope = decode(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"bind-ack\"," 
        "\"type\":\"pairing_bind_ack\",\"payload\":{\"pairing_request_id\":"
        "\"pairing-request\",\"device_id\":\"device-paired\",\"host_proof\":"
        "\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}");
    device_pairing_bind_ack_t ack = {0};
    assert(device_pairing_decode_bind_ack(envelope, "pairing-request", &ack) ==
           DEVICE_PAIRING_PROTOCOL_RESULT_OK);
    assert(strcmp(ack.device_id, "device-paired") == 0);
    device_control_envelope_release(envelope);

    envelope = decode(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"complete\"," 
        "\"type\":\"pairing_complete\",\"payload\":{\"pairing_request_id\":"
        "\"pairing-request\",\"device_id\":\"device-paired\",\"display_name\":"
        "\"Agent Robot\"}}");
    device_pairing_complete_t complete = {0};
    assert(device_pairing_decode_complete(
               envelope,
               "pairing-request",
               "device-paired",
               &complete) == DEVICE_PAIRING_PROTOCOL_RESULT_OK);
    assert(strcmp(complete.display_name, "Agent Robot") == 0);
    assert(device_pairing_decode_complete(
               envelope,
               "pairing-request",
               "different-device",
               &complete) == DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE);
    device_control_envelope_release(envelope);

    envelope = decode(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"bad\"," 
        "\"type\":\"pairing_bind_ack\",\"payload\":{\"pairing_request_id\":"
        "\"pairing-request\",\"device_id\":\"device-paired\",\"host_proof\":"
        "\"not-base64!\",\"extra\":true}}");
    assert(device_pairing_decode_bind_ack(envelope, "pairing-request", &ack) ==
           DEVICE_PAIRING_PROTOCOL_RESULT_INVALID_MESSAGE);
    device_control_envelope_release(envelope);
}

int main(void)
{
    verify_client_messages();
    verify_host_messages();
    puts("device pairing strict payload tests passed");
    return 0;
}
