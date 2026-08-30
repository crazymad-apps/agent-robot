#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_control_envelope.h"
#include "device_pairing.h"

typedef struct {
    uint64_t sequence;
} deterministic_random_t;

static bool deterministic_random_fill(void *context, uint8_t *output, size_t output_size)
{
    deterministic_random_t *random = context;
    for (size_t index = 0U; index < output_size; ++index) {
        random->sequence ^= random->sequence << 13U;
        random->sequence ^= random->sequence >> 7U;
        random->sequence ^= random->sequence << 17U;
        output[index] = (uint8_t)random->sequence;
    }
    return true;
}

device_identity_result_t device_identity_save_pending(
    device_identity_t *identity,
    const device_identity_record_t *record)
{
    (void)identity;
    (void)record;
    return DEVICE_IDENTITY_RESULT_OK;
}

device_identity_result_t device_identity_activate(
    device_identity_t *identity,
    const char *expected_device_id)
{
    (void)identity;
    (void)expected_device_id;
    return DEVICE_IDENTITY_RESULT_OK;
}

static device_control_envelope_t *decode(const char *encoded)
{
    device_control_envelope_t *envelope = NULL;
    assert(device_control_envelope_decode(encoded, strlen(encoded), &envelope) ==
           DEVICE_PROTOCOL_RESULT_OK);
    assert(envelope != NULL);
    return envelope;
}

static void pairing_request_copy(
    const char *encoded,
    char request[DEVICE_PAIRING_REQUEST_ID_MAX_BYTES + 1U])
{
    device_control_envelope_t *envelope = decode(encoded);
    assert(strcmp(device_control_envelope_message_type(envelope), "pairing_hello") == 0);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(
        device_control_envelope_payload(envelope),
        "pairing_request_id");
    assert(cJSON_IsString(value));
    assert(strlen(value->valuestring) <= DEVICE_PAIRING_REQUEST_ID_MAX_BYTES);
    strcpy(request, value->valuestring);
    device_control_envelope_release(envelope);
}

static device_pairing_result_t handle_error(
    device_pairing_t *pairing,
    const char *code,
    bool recoverable,
    device_pairing_transition_t *transition)
{
    char encoded[512];
    const int written = snprintf(
        encoded,
        sizeof(encoded),
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"error-message\"," 
        "\"type\":\"error\",\"payload\":{\"code\":\"%s\"," 
        "\"correlation_message_id\":null,\"recoverable\":%s}}",
        code,
        recoverable ? "true" : "false");
    assert(written > 0 && (size_t)written < sizeof(encoded));
    device_control_envelope_t *envelope = decode(encoded);
    char first[2048];
    char second[2048];
    const device_pairing_result_t result = device_pairing_handle_control(
        pairing,
        envelope,
        first,
        sizeof(first),
        second,
        sizeof(second),
        transition);
    device_control_envelope_release(envelope);
    return result;
}

int main(void)
{
    deterministic_random_t random = {.sequence = 0x6a09e667f3bcc909ULL};
    const device_capabilities_t capabilities = {
        .input_pcm16_16k_mono = true,
        .output_pcm16_16k_mono = true,
        .playback_cancel = true,
        .display_status = true,
    };
    const device_pairing_config_t config = {
        .identity_store = (device_identity_t *)(uintptr_t)1U,
        .capabilities = capabilities,
        .display_name = "Agent Robot Terminal",
        .random_source = {
            .fill = deterministic_random_fill,
            .context = &random,
        },
    };
    device_pairing_t *pairing = NULL;
    assert(device_pairing_create(&config, &pairing) == DEVICE_PAIRING_RESULT_OK);

    char initial_code[DEVICE_PAIRING_CODE_CAPACITY];
    assert(device_pairing_copy_code(pairing, initial_code) == DEVICE_PAIRING_RESULT_OK);
    assert(strlen(initial_code) == DEVICE_PAIRING_CODE_DIGITS);
    for (size_t index = 0U; index < DEVICE_PAIRING_CODE_DIGITS; ++index) {
        assert(initial_code[index] >= '0' && initial_code[index] <= '9');
    }

    const host_discovery_record_t host = {
        .ipv4_address = 0x0100007fU,
        .port = 443U,
        .protocol_major = 1U,
        .protocol_minor = 0U,
        .path = "/device",
        .installation_id = "0123456789abcdef0123456789abcdef",
        .certificate_fingerprint =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .pairing_available = true,
    };
    char hello[2048];
    size_t hello_size = 0U;
    assert(device_pairing_begin(pairing, &host, hello, sizeof(hello), &hello_size) ==
           DEVICE_PAIRING_RESULT_OK);
    char first_request[DEVICE_PAIRING_REQUEST_ID_MAX_BYTES + 1U] = {0};
    pairing_request_copy(hello, first_request);

    device_pairing_transition_t transition;
    assert(device_pairing_connection_lost(pairing, true, &transition) ==
           DEVICE_PAIRING_RESULT_OK);
    assert(!transition.attempt_reset);
    assert(device_pairing_begin(pairing, &host, hello, sizeof(hello), &hello_size) ==
           DEVICE_PAIRING_RESULT_OK);
    char resumed_request[DEVICE_PAIRING_REQUEST_ID_MAX_BYTES + 1U] = {0};
    pairing_request_copy(hello, resumed_request);
    assert(strcmp(first_request, resumed_request) == 0);
    char resumed_code[DEVICE_PAIRING_CODE_CAPACITY];
    assert(device_pairing_copy_code(pairing, resumed_code) == DEVICE_PAIRING_RESULT_OK);
    assert(strcmp(initial_code, resumed_code) == 0);

    assert(device_pairing_connection_lost(pairing, false, &transition) ==
           DEVICE_PAIRING_RESULT_OK);
    assert(transition.attempt_reset);
    assert(device_pairing_state(pairing) == DEVICE_PAIRING_STATE_READY);
    assert(device_pairing_begin(pairing, &host, hello, sizeof(hello), &hello_size) ==
           DEVICE_PAIRING_RESULT_OK);
    char replacement_request[DEVICE_PAIRING_REQUEST_ID_MAX_BYTES + 1U] = {0};
    pairing_request_copy(hello, replacement_request);
    assert(strcmp(first_request, replacement_request) != 0);
    assert(device_pairing_copy_code(pairing, resumed_code) == DEVICE_PAIRING_RESULT_OK);
    assert(strcmp(initial_code, resumed_code) == 0);

    assert(handle_error(pairing, "pairing_not_open", true, &transition) ==
           DEVICE_PAIRING_RESULT_OK);
    assert(transition.attempt_reset);
    assert(device_pairing_security_failure_count(pairing) == 0U);

    for (size_t attempt = 0U; attempt < DEVICE_PAIRING_MAX_SECURITY_FAILURES; ++attempt) {
        assert(device_pairing_begin(pairing, &host, hello, sizeof(hello), &hello_size) ==
               DEVICE_PAIRING_RESULT_OK);
        assert(handle_error(pairing, "pairing_failed", true, &transition) ==
               DEVICE_PAIRING_RESULT_OK);
        assert(transition.attempt_reset);
        assert(transition.pairing_code_rotated ==
               (attempt + 1U == DEVICE_PAIRING_MAX_SECURITY_FAILURES));
    }
    char rotated_code[DEVICE_PAIRING_CODE_CAPACITY];
    assert(device_pairing_copy_code(pairing, rotated_code) == DEVICE_PAIRING_RESULT_OK);
    assert(strcmp(initial_code, rotated_code) != 0);
    assert(device_pairing_security_failure_count(pairing) == 0U);

    assert(device_pairing_begin(pairing, &host, hello, sizeof(hello), &hello_size) ==
           DEVICE_PAIRING_RESULT_OK);
    assert(handle_error(pairing, "pairing_failed", false, &transition) ==
           DEVICE_PAIRING_RESULT_PROTOCOL_ERROR);
    assert(device_pairing_state(pairing) == DEVICE_PAIRING_STATE_HELLO_SENT);

    device_pairing_destroy(pairing);
    puts("device pairing state and reconnect tests passed");
    return 0;
}
