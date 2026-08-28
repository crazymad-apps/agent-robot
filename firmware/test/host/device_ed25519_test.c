#include "device_base64url.h"
#include "device_ed25519.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

int main(int argument_count, char **arguments)
{
    assert(argument_count == 5);
    uint8_t transcript[512] = {0};
    uint8_t seed[DEVICE_ED25519_SEED_BYTES] = {0};
    uint8_t expected_public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES] = {0};
    uint8_t expected_signature[DEVICE_ED25519_SIGNATURE_BYTES] = {0};
    const size_t transcript_size =
        decode_fixture(arguments[1], transcript, sizeof(transcript));
    assert(decode_fixture(arguments[2], seed, sizeof(seed)) == sizeof(seed));
    assert(decode_fixture(arguments[3], expected_public_key, sizeof(expected_public_key))
           == sizeof(expected_public_key));
    assert(decode_fixture(arguments[4], expected_signature, sizeof(expected_signature))
           == sizeof(expected_signature));

    uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES] = {0};
    uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES] = {0};
    assert(device_ed25519_derive_public_key(seed, public_key) == DEVICE_CRYPTO_RESULT_OK);
    assert(memcmp(public_key, expected_public_key, sizeof(public_key)) == 0);
    assert(device_ed25519_sign(seed, transcript, transcript_size, signature)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(memcmp(signature, expected_signature, sizeof(signature)) == 0);
    assert(device_ed25519_verify(public_key, transcript, transcript_size, signature)
           == DEVICE_CRYPTO_RESULT_OK);

    signature[0] ^= 1U;
    assert(device_ed25519_verify(public_key, transcript, transcript_size, signature)
           == DEVICE_CRYPTO_RESULT_INVALID_SIGNATURE);
    signature[0] ^= 1U;
    transcript[0] ^= 1U;
    assert(device_ed25519_verify(public_key, transcript, transcript_size, signature)
           == DEVICE_CRYPTO_RESULT_INVALID_SIGNATURE);

    puts("device Ed25519 fixture and tamper checks passed");
    return 0;
}
