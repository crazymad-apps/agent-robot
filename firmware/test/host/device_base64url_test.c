#include "device_base64url.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void verify_fixture_value(const char *encoded, size_t expected_decoded_size)
{
    uint8_t decoded[512] = {0};
    size_t decoded_size = 0U;
    assert(device_base64url_decode(
               encoded,
               strlen(encoded),
               decoded,
               sizeof(decoded),
               &decoded_size)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(decoded_size == expected_decoded_size);

    char round_trip[768] = {0};
    size_t round_trip_size = 0U;
    assert(device_base64url_encode(
               decoded,
               decoded_size,
               round_trip,
               sizeof(round_trip),
               &round_trip_size)
           == DEVICE_CRYPTO_RESULT_OK);
    assert(round_trip_size == strlen(encoded));
    assert(memcmp(round_trip, encoded, round_trip_size) == 0);
}

int main(int argument_count, char **arguments)
{
    assert(argument_count > 1 && argument_count % 2 == 1);
    for (int index = 1; index < argument_count; index += 2) {
        verify_fixture_value(arguments[index], (size_t)strtoul(arguments[index + 1], NULL, 10));
    }

    uint8_t decoded[8] = {0};
    size_t decoded_size = 0U;
    assert(device_base64url_decode("A", 1U, decoded, sizeof(decoded), &decoded_size)
           == DEVICE_CRYPTO_RESULT_INVALID_ENCODING);
    assert(device_base64url_decode("AA==", 4U, decoded, sizeof(decoded), &decoded_size)
           == DEVICE_CRYPTO_RESULT_INVALID_ENCODING);
    assert(device_base64url_decode("AB", 2U, decoded, sizeof(decoded), &decoded_size)
           == DEVICE_CRYPTO_RESULT_INVALID_ENCODING);

    char encoded[1] = {0};
    size_t encoded_size = 0U;
    assert(device_base64url_encode(decoded, sizeof(decoded), encoded, sizeof(encoded), &encoded_size)
           == DEVICE_CRYPTO_RESULT_OUTPUT_TOO_SMALL);
    assert(encoded_size == 11U);

    puts("device base64url fixture and strict encoding checks passed");
    return 0;
}
