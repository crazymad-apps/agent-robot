#include "device_base64url.h"
#include "device_spake2.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/md.h"

typedef struct {
    uint8_t private_scalar[32];
    uint64_t sequence;
    bool scalar_supplied;
} deterministic_random_t;

static uint8_t hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return (uint8_t)(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return (uint8_t)(value - 'a' + 10);
    }
    assert(false);
    return 0U;
}

static void decode_hex_scalar(const char *encoded, uint8_t decoded[32])
{
    assert(strlen(encoded) == 64U);
    for (size_t index = 0U; index < 32U; ++index) {
        decoded[index] = (uint8_t)(
            (hex_nibble(encoded[index * 2U]) << 4U)
            | hex_nibble(encoded[index * 2U + 1U]));
    }
}

static bool deterministic_random_fill(void *context, uint8_t *output, size_t output_size)
{
    deterministic_random_t *random = context;
    if (!random->scalar_supplied && output_size == sizeof(random->private_scalar)) {
        memcpy(output, random->private_scalar, output_size);
        random->scalar_supplied = true;
        return true;
    }
    for (size_t index = 0U; index < output_size; ++index) {
        random->sequence ^= random->sequence << 13U;
        random->sequence ^= random->sequence >> 7U;
        random->sequence ^= random->sequence << 17U;
        output[index] = (uint8_t)random->sequence;
    }
    return true;
}

static bool unavailable_random_fill(void *context, uint8_t *output, size_t output_size)
{
    (void)context;
    (void)output;
    (void)output_size;
    return false;
}

static size_t decode_fixture(
    const char *encoded,
    uint8_t *decoded,
    size_t decoded_capacity)
{
    size_t decoded_size = 0U;
    assert(device_base64url_decode(
               encoded,
               strlen(encoded),
               decoded,
               decoded_capacity,
               &decoded_size)
           == DEVICE_CRYPTO_RESULT_OK);
    return decoded_size;
}

static void direct_binding_mac(
    const uint8_t session_key[16],
    const uint8_t *label,
    size_t label_size,
    const uint8_t *transcript,
    size_t transcript_size,
    uint8_t output[32])
{
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    assert(sha256 != NULL);
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    assert(mbedtls_md_setup(&context, sha256, 1) == 0);
    assert(mbedtls_md_hmac_starts(&context, session_key, 16U) == 0);
    assert(mbedtls_md_hmac_update(&context, label, label_size) == 0);
    assert(mbedtls_md_hmac_update(&context, transcript, transcript_size) == 0);
    assert(mbedtls_md_hmac_finish(&context, output) == 0);
    mbedtls_md_free(&context);
}

int main(int argument_count, char **arguments)
{
    assert(argument_count == 11);
    deterministic_random_t random = {.sequence = 0x243f6a8885a308d3ULL};
    decode_hex_scalar(arguments[4], random.private_scalar);
    const device_random_source_t random_source = {
        .fill = deterministic_random_fill,
        .context = &random,
    };

    uint8_t expected_device_share[DEVICE_SPAKE2_SHARE_BYTES] = {0};
    uint8_t host_share[DEVICE_SPAKE2_SHARE_BYTES] = {0};
    uint8_t associated_data[DEVICE_SPAKE2_ASSOCIATED_DATA_MAX_BYTES] = {0};
    uint8_t expected_device_confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES] = {0};
    uint8_t host_confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES] = {0};
    uint8_t session_key[16] = {0};
    assert(decode_fixture(arguments[5], expected_device_share, sizeof(expected_device_share))
           == sizeof(expected_device_share));
    assert(decode_fixture(arguments[6], host_share, sizeof(host_share)) == sizeof(host_share));
    const size_t associated_data_size =
        decode_fixture(arguments[7], associated_data, sizeof(associated_data));
    assert(decode_fixture(
               arguments[8], expected_device_confirmation, sizeof(expected_device_confirmation))
           == sizeof(expected_device_confirmation));
    assert(decode_fixture(arguments[9], host_confirmation, sizeof(host_confirmation))
           == sizeof(host_confirmation));
    assert(decode_fixture(arguments[10], session_key, sizeof(session_key)) == sizeof(session_key));

    device_spake2_party_a_t *session = NULL;
    uint8_t device_share[DEVICE_SPAKE2_SHARE_BYTES] = {0};
    assert(device_spake2_party_a_begin(
               arguments[1],
               arguments[2],
               arguments[3],
               &random_source,
               &session,
               device_share)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(session != NULL);
    assert(memcmp(device_share, expected_device_share, sizeof(device_share)) == 0);

    device_spake2_keys_t *keys = NULL;
    assert(device_spake2_party_a_finish(
               session,
               host_share,
               associated_data,
               associated_data_size,
               &keys)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(keys != NULL);
    uint8_t device_confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES] = {0};
    assert(device_spake2_keys_copy_device_confirmation(keys, device_confirmation)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(memcmp(
               device_confirmation,
               expected_device_confirmation,
               sizeof(device_confirmation))
           == 0);
    assert(device_spake2_keys_verify_host_confirmation(keys, host_confirmation)
           == DEVICE_CRYPTO_RESULT_OK);
    host_confirmation[0] ^= 1U;
    assert(device_spake2_keys_verify_host_confirmation(keys, host_confirmation)
           == DEVICE_CRYPTO_RESULT_CONFIRMATION_FAILED);

    static const uint8_t label[] = "binding-check";
    static const uint8_t binding_transcript[] = "fixture-transcript";
    uint8_t expected_binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES] = {0};
    uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES] = {0};
    direct_binding_mac(
        session_key,
        label,
        sizeof(label) - 1U,
        binding_transcript,
        sizeof(binding_transcript) - 1U,
        expected_binding_mac);
    assert(device_spake2_keys_binding_mac(
               keys,
               label,
               sizeof(label) - 1U,
               binding_transcript,
               sizeof(binding_transcript) - 1U,
               binding_mac)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(memcmp(binding_mac, expected_binding_mac, sizeof(binding_mac)) == 0);
    assert(device_spake2_keys_verify_binding_mac(
               keys,
               label,
               sizeof(label) - 1U,
               binding_transcript,
               sizeof(binding_transcript) - 1U,
               binding_mac)
           == DEVICE_CRYPTO_RESULT_OK);
    binding_mac[0] ^= 1U;
    assert(device_spake2_keys_verify_binding_mac(
               keys,
               label,
               sizeof(label) - 1U,
               binding_transcript,
               sizeof(binding_transcript) - 1U,
               binding_mac)
           == DEVICE_CRYPTO_RESULT_BINDING_FAILED);

    device_spake2_keys_release(keys);
    device_spake2_party_a_release(session);

    random.scalar_supplied = false;
    session = NULL;
    assert(device_spake2_party_a_begin(
               arguments[1],
               arguments[2],
               arguments[3],
               &random_source,
               &session,
               device_share)
           == DEVICE_CRYPTO_RESULT_OK);
    uint8_t invalid_host_share[DEVICE_SPAKE2_SHARE_BYTES];
    memcpy(invalid_host_share, host_share, sizeof(invalid_host_share));
    invalid_host_share[0] = 0x05U;
    keys = NULL;
    assert(device_spake2_party_a_finish(
               session,
               invalid_host_share,
               associated_data,
               associated_data_size,
               &keys)
           == DEVICE_CRYPTO_RESULT_INVALID_POINT);
    assert(keys == NULL);
    assert(device_spake2_party_a_finish(
               session,
               host_share,
               associated_data,
               associated_data_size,
               &keys)
           == DEVICE_CRYPTO_RESULT_INVALID_STATE);
    device_spake2_party_a_release(session);

    assert(device_spake2_party_a_begin(
               "12345678",
               arguments[2],
               arguments[3],
               &random_source,
               &session,
               device_share)
           == DEVICE_CRYPTO_RESULT_INVALID_ARGUMENT);

    const device_random_source_t unavailable_random = {
        .fill = unavailable_random_fill,
    };
    assert(device_spake2_party_a_begin(
               arguments[1],
               arguments[2],
               arguments[3],
               &unavailable_random,
               &session,
               device_share)
           == DEVICE_CRYPTO_RESULT_RANDOM_FAILED);
    assert(session == NULL);

    puts("device SPAKE2 P-256 share, key schedule, and confirmation fixtures passed");
    return 0;
}
