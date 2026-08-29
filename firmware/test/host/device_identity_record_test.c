#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_ed25519.h"
#include "device_identity_record.h"

static const char *const INSTALLATION_ID =
    "0123456789abcdef0123456789abcdef";
static const char *const CERTIFICATE_FINGERPRINT =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static device_identity_record_t pending_record(void)
{
    device_identity_record_t record = {
        .state = DEVICE_IDENTITY_STATE_PENDING,
        .private_seed = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        },
    };
    strcpy(record.device_id, "device-persistence-boundary");
    strcpy(record.host_installation_id, INSTALLATION_ID);
    strcpy(record.host_certificate_fingerprint, CERTIFICATE_FINGERPRINT);
    assert(device_ed25519_derive_public_key(record.private_seed, record.public_key) ==
           DEVICE_CRYPTO_RESULT_OK);
    return record;
}

static void round_trip_pending_and_active(void)
{
    device_identity_record_t source = pending_record();
    uint8_t encoded[DEVICE_IDENTITY_RECORD_ENCODED_BYTES];
    size_t encoded_size = 0U;
    assert(device_identity_record_encode(
               &source,
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_IDENTITY_RESULT_OK);
    assert(encoded_size == sizeof(encoded));

    device_identity_record_t decoded = {0};
    assert(device_identity_record_decode(encoded, encoded_size, &decoded) ==
           DEVICE_IDENTITY_RESULT_OK);
    assert(memcmp(&source, &decoded, sizeof(source)) == 0);

    source.state = DEVICE_IDENTITY_STATE_ACTIVE;
    assert(device_identity_record_encode(
               &source,
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_IDENTITY_RESULT_OK);
    assert(device_identity_record_decode(encoded, encoded_size, &decoded) ==
           DEVICE_IDENTITY_RESULT_OK);
    assert(decoded.state == DEVICE_IDENTITY_STATE_ACTIVE);
}

static void round_trip_retained_host_binding(void)
{
    device_identity_record_t source = {0};
    strcpy(source.host_installation_id, INSTALLATION_ID);
    strcpy(source.host_certificate_fingerprint, CERTIFICATE_FINGERPRINT);
    uint8_t encoded[DEVICE_IDENTITY_RECORD_ENCODED_BYTES];
    size_t encoded_size = 0U;
    assert(device_identity_record_encode(
               &source,
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_IDENTITY_RESULT_OK);
    device_identity_record_t decoded = {0};
    assert(device_identity_record_decode(encoded, encoded_size, &decoded) ==
           DEVICE_IDENTITY_RESULT_OK);
    assert(decoded.state == DEVICE_IDENTITY_STATE_NONE);
    assert(strcmp(decoded.host_installation_id, INSTALLATION_ID) == 0);
    assert(strcmp(decoded.host_certificate_fingerprint, CERTIFICATE_FINGERPRINT) == 0);
}

static void reject_invalid_records(void)
{
    device_identity_record_t record = pending_record();
    record.public_key[0] ^= 0x80U;
    assert(device_identity_record_validate(&record) == DEVICE_IDENTITY_RESULT_CORRUPT);

    record = pending_record();
    record.host_installation_id[0] = 'A';
    assert(device_identity_record_validate(&record) == DEVICE_IDENTITY_RESULT_CORRUPT);

    record = pending_record();
    strcpy(record.device_id, " \t\n");
    assert(device_identity_record_validate(&record) == DEVICE_IDENTITY_RESULT_CORRUPT);

    record = pending_record();
    record.state = DEVICE_IDENTITY_STATE_NONE;
    assert(device_identity_record_validate(&record) == DEVICE_IDENTITY_RESULT_CORRUPT);
}

static void reject_corrupt_encoding(void)
{
    device_identity_record_t source = pending_record();
    uint8_t encoded[DEVICE_IDENTITY_RECORD_ENCODED_BYTES];
    size_t encoded_size = 0U;
    assert(device_identity_record_encode(
               &source,
               encoded,
               sizeof(encoded),
               &encoded_size) == DEVICE_IDENTITY_RESULT_OK);
    device_identity_record_t decoded = {0};

    encoded[4] = 2U;
    assert(device_identity_record_decode(encoded, encoded_size, &decoded) ==
           DEVICE_IDENTITY_RESULT_CORRUPT);
    encoded[4] = DEVICE_IDENTITY_RECORD_SCHEMA_VERSION;

    encoded[9] = 1U;
    assert(device_identity_record_decode(encoded, encoded_size, &decoded) ==
           DEVICE_IDENTITY_RESULT_CORRUPT);
    encoded[9] = 0U;

    assert(device_identity_record_decode(encoded, encoded_size - 1U, &decoded) ==
           DEVICE_IDENTITY_RESULT_CORRUPT);

    encoded[encoded_size - 1U] ^= 0x01U;
    assert(device_identity_record_decode(encoded, encoded_size, &decoded) ==
           DEVICE_IDENTITY_RESULT_CORRUPT);
}

int main(void)
{
    round_trip_pending_and_active();
    round_trip_retained_host_binding();
    reject_invalid_records();
    reject_corrupt_encoding();
    puts("device identity record tests passed");
    return 0;
}
