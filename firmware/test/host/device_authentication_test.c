#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_authentication.h"
#include "device_base64url.h"
#include "device_control_envelope.h"
#include "device_ed25519.h"

static const device_capabilities_t CAPABILITIES = {
    .input_text = true,
    .input_pcm16_16k_mono = false,
    .output_text = true,
    .output_pcm16_16k_mono = false,
    .playback_cancel = false,
    .display_status = true,
    .display_transcript = true,
};

static device_control_envelope_t *decode(const char *encoded)
{
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(encoded, strlen(encoded), &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(envelope != NULL);
    return envelope;
}

static device_identity_record_t fixture_identity(
    const char *private_seed,
    const char *public_key)
{
    device_identity_record_t identity = {.state = DEVICE_IDENTITY_STATE_ACTIVE};
    strcpy(identity.device_id, "device-fixture");
    strcpy(identity.host_installation_id, "0123456789abcdef0123456789abcdef");
    strcpy(
        identity.host_certificate_fingerprint,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    size_t decoded_size = 0U;
    assert(device_base64url_decode(
               private_seed,
               strlen(private_seed),
               identity.private_seed,
               sizeof(identity.private_seed),
               &decoded_size) == DEVICE_CRYPTO_RESULT_OK);
    assert(decoded_size == sizeof(identity.private_seed));
    assert(device_base64url_decode(
               public_key,
               strlen(public_key),
               identity.public_key,
               sizeof(identity.public_key),
               &decoded_size) == DEVICE_CRYPTO_RESULT_OK);
    assert(decoded_size == sizeof(identity.public_key));
    return identity;
}

static void verify_challenge(void)
{
    const char *valid =
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"challenge-1\","
        "\"type\":\"auth_challenge\",\"payload\":{\"connection_id\":\"connection-fixture\","
        "\"nonce\":\"host-auth-nonce\","
        "\"server_time_ms\":1787790000000}}";
    device_control_envelope_t *envelope = decode(valid);
    device_authentication_challenge_t challenge = {0};
    assert(device_authentication_decode_challenge(envelope, &challenge) ==
           DEVICE_AUTHENTICATION_RESULT_OK);
    assert(strcmp(challenge.connection_id, "connection-fixture") == 0);
    assert(challenge.server_time_ms == 1787790000000LL);
    device_control_envelope_release(envelope);

    const char *unknown =
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"challenge-2\","
        "\"type\":\"auth_challenge\",\"payload\":{\"connection_id\":\"connection-fixture\","
        "\"nonce\":\"host-auth-nonce\","
        "\"server_time_ms\":1787790000000,\"extra\":true}}";
    envelope = decode(unknown);
    assert(device_authentication_decode_challenge(envelope, &challenge) ==
           DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE);
    device_control_envelope_release(envelope);
}

static void verify_hello_signature(
    const char *private_seed,
    const char *public_key,
    const char *expected_signature)
{
    const device_identity_record_t identity = fixture_identity(private_seed, public_key);
    const device_authentication_challenge_t challenge = {
        .connection_id = "connection-fixture",
        .host_nonce = "host-auth-nonce",
        .server_time_ms = 1787790000000LL,
    };
    char encoded[2048];
    size_t encoded_size = 0U;
    assert(device_authentication_encode_hello(
               "hello-1",
               &challenge,
               &identity,
               "device-auth-nonce",
               &CAPABILITIES,
               DEVICE_OUTPUT_PREFERENCE_TEXT,
               "agent-robot/3.1",
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_AUTHENTICATION_RESULT_OK);
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(encoded, encoded_size, &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(strcmp(device_control_envelope_message_type(envelope), "hello") == 0);
    const cJSON *payload = device_control_envelope_payload(envelope);
    assert(strcmp(
               cJSON_GetObjectItemCaseSensitive(payload, "device_id")->valuestring,
               "device-fixture") == 0);
    assert(strcmp(
               cJSON_GetObjectItemCaseSensitive(payload, "signature")->valuestring,
               expected_signature) == 0);
    device_control_envelope_release(envelope);
}

static void verify_ack_and_errors(void)
{
    const char *ack_text =
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"ack-1\","
        "\"type\":\"hello_ack\",\"payload\":{\"device_id\":\"device-fixture\","
        "\"connection_id\":\"connection-fixture\",\"capabilities\":{"
        "\"input_text\":true,\"input_pcm16_16k_mono\":false,\"output_text\":true,"
        "\"output_pcm16_16k_mono\":false,\"playback_cancel\":false,"
        "\"display_status\":true,\"display_transcript\":true},"
        "\"output_preference\":\"text\"}}";
    device_control_envelope_t *envelope = decode(ack_text);
    device_authentication_ack_t ack = {0};
    assert(device_authentication_decode_ack(
               envelope,
               "device-fixture",
               "connection-fixture",
               &ack) == DEVICE_AUTHENTICATION_RESULT_OK);
    assert(ack.capabilities.input_text && ack.capabilities.output_text);
    assert(ack.output_preference == DEVICE_OUTPUT_PREFERENCE_TEXT);
    assert(device_authentication_decode_ack(
               envelope,
               "different-device",
               "connection-fixture",
               &ack) == DEVICE_AUTHENTICATION_RESULT_INVALID_MESSAGE);
    device_control_envelope_release(envelope);

    const char *error_text =
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"error-1\","
        "\"type\":\"error\",\"payload\":{\"code\":\"device_revoked\","
        "\"correlation_message_id\":null,\"recoverable\":false}}";
    envelope = decode(error_text);
    device_authentication_error_t error = {0};
    assert(device_authentication_decode_error(envelope, &error) ==
           DEVICE_AUTHENTICATION_RESULT_OK);
    assert(strcmp(error.code, "device_revoked") == 0);
    assert(!error.has_correlation_message_id && !error.recoverable);
    device_control_envelope_release(envelope);
}

int main(int argument_count, char **arguments)
{
    assert(argument_count == 4);
    verify_challenge();
    verify_hello_signature(arguments[1], arguments[2], arguments[3]);
    verify_ack_and_errors();
    puts("device authentication payload and signature tests passed");
    return 0;
}
