#include "device_base64url.h"
#include "device_capabilities.h"
#include "device_transcript.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static device_capabilities_t capabilities_from_bits(const char *bits)
{
    assert(strlen(bits) == DEVICE_CAPABILITIES_BYTES);
    for (size_t index = 0U; index < DEVICE_CAPABILITIES_BYTES; ++index) {
        assert(bits[index] == '0' || bits[index] == '1');
    }
    return (device_capabilities_t) {
        .input_text = bits[0] == '1',
        .input_pcm16_16k_mono = bits[1] == '1',
        .output_text = bits[2] == '1',
        .output_pcm16_16k_mono = bits[3] == '1',
        .playback_cancel = bits[4] == '1',
        .display_status = bits[5] == '1',
        .display_transcript = bits[6] == '1',
    };
}

static size_t decode_expected(const char *encoded, uint8_t *expected, size_t expected_capacity)
{
    size_t expected_size = 0U;
    assert(device_base64url_decode(
               encoded,
               strlen(encoded),
               expected,
               expected_capacity,
               &expected_size)
           == DEVICE_CRYPTO_RESULT_OK);
    return expected_size;
}

static void verify_pairing_associated_data(char **arguments, const device_capabilities_t *capabilities)
{
    uint8_t expected[512] = {0};
    const size_t expected_size = decode_expected(arguments[1], expected, sizeof(expected));
    size_t required_size = 0U;
    assert(device_pairing_associated_data_encode(
               arguments[2],
               arguments[3],
               arguments[4],
               arguments[5],
               arguments[6],
               capabilities,
               NULL,
               0U,
               &required_size)
           == DEVICE_CRYPTO_RESULT_OUTPUT_TOO_SMALL);
    assert(required_size == expected_size);

    uint8_t actual[512] = {0};
    size_t actual_size = 0U;
    assert(device_pairing_associated_data_encode(
               arguments[2],
               arguments[3],
               arguments[4],
               arguments[5],
               arguments[6],
               capabilities,
               actual,
               sizeof(actual),
               &actual_size)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(actual_size == expected_size);
    assert(memcmp(actual, expected, actual_size) == 0);
}

static void verify_authentication_transcript(
    char **arguments,
    const device_capabilities_t *capabilities)
{
    uint8_t expected[512] = {0};
    const size_t expected_size = decode_expected(arguments[8], expected, sizeof(expected));
    uint8_t actual[512] = {0};
    size_t actual_size = 0U;
    assert(strcmp(arguments[13], "text") == 0);
    assert(device_authentication_transcript_encode(
               arguments[9],
               arguments[10],
               arguments[11],
               arguments[12],
               capabilities,
               DEVICE_OUTPUT_PREFERENCE_TEXT,
               actual,
               sizeof(actual),
               &actual_size)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(actual_size == expected_size);
    assert(memcmp(actual, expected, actual_size) == 0);

    assert(device_authentication_transcript_encode(
               arguments[9],
               arguments[10],
               arguments[11],
               arguments[12],
               capabilities,
               (device_output_preference_t)99,
               actual,
               sizeof(actual),
               &actual_size)
           == DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT);
}

int main(int argument_count, char **arguments)
{
    assert(argument_count == 14);
    const device_capabilities_t capabilities = capabilities_from_bits(arguments[7]);
    uint8_t capability_bytes[DEVICE_CAPABILITIES_BYTES] = {0};
    assert(device_capabilities_serialize(&capabilities, capability_bytes)
           == DEVICE_PROTOCOL_RESULT_OK);
    for (size_t index = 0U; index < DEVICE_CAPABILITIES_BYTES; ++index) {
        assert(capability_bytes[index] == (uint8_t)(arguments[7][index] - '0'));
    }

    verify_pairing_associated_data(arguments, &capabilities);
    verify_authentication_transcript(arguments, &capabilities);

    uint8_t password_transcript[256] = {0};
    size_t password_transcript_size = 0U;
    assert(device_pairing_password_transcript_encode(
               "123456",
               arguments[2],
               arguments[3],
               password_transcript,
               sizeof(password_transcript),
               &password_transcript_size)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(password_transcript_size > 0U);
    assert(device_pairing_password_transcript_encode(
               "12345678",
               arguments[2],
               arguments[3],
               password_transcript,
               sizeof(password_transcript),
               &password_transcript_size)
           == DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT);

    puts("device capability and transcript fixtures passed");
    return 0;
}
