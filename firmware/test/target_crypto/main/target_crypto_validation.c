#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_base64url.h"
#include "device_capabilities.h"
#include "device_control_envelope.h"
#include "device_crypto_fixture_data.h"
#include "device_ed25519.h"
#include "device_hardware_random.h"
#include "device_pcm_frame.h"
#include "device_spake2.h"
#include "device_transcript.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

typedef struct {
    uint8_t private_scalar[32];
    uint64_t sequence;
    bool scalar_supplied;
} deterministic_random_t;

static const char *const TAG = "crypto_validation";

static void require_validation(bool condition, const char *step)
{
    if (!condition) {
        ESP_LOGE(TAG, "validation failed: %s", step);
        vTaskSuspend(NULL);
        abort();
    }
}

static const cJSON *require_object_item(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    require_validation(cJSON_IsObject(item), name);
    return item;
}

static const char *require_string_item(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    require_validation(cJSON_IsString(item) && item->valuestring != NULL, name);
    return item->valuestring;
}

static bool require_boolean_item(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    require_validation(cJSON_IsBool(item), name);
    return cJSON_IsTrue(item);
}

static size_t decode_base64url(
    const char *encoded,
    uint8_t *decoded,
    size_t decoded_capacity,
    const char *step)
{
    size_t decoded_size = 0U;
    require_validation(
        device_base64url_decode(
            encoded,
            strlen(encoded),
            decoded,
            decoded_capacity,
            &decoded_size)
            == DEVICE_CRYPTO_RESULT_OK,
        step);
    return decoded_size;
}

static uint8_t decode_hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return (uint8_t)(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return (uint8_t)(value - 'a' + 10);
    }
    require_validation(false, "private scalar encoding");
    return 0U;
}

static void decode_private_scalar(const char *encoded, uint8_t decoded[32])
{
    require_validation(strlen(encoded) == 64U, "private scalar size");
    for (size_t index = 0U; index < 32U; ++index) {
        decoded[index] = (uint8_t)(
            (decode_hex_nibble(encoded[index * 2U]) << 4U)
            | decode_hex_nibble(encoded[index * 2U + 1U]));
    }
}

static void decode_hex(
    const char *encoded,
    uint8_t *decoded,
    size_t decoded_size,
    const char *step)
{
    require_validation(strlen(encoded) == decoded_size * 2U, step);
    for (size_t index = 0U; index < decoded_size; ++index) {
        decoded[index] = (uint8_t)(
            (decode_hex_nibble(encoded[index * 2U]) << 4U)
            | decode_hex_nibble(encoded[index * 2U + 1U]));
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

static device_capabilities_t parse_capabilities(const cJSON *pairing)
{
    const cJSON *capabilities = require_object_item(pairing, "capabilities");
    return (device_capabilities_t) {
        .input_text = require_boolean_item(capabilities, "input_text"),
        .input_pcm16_16k_mono =
            require_boolean_item(capabilities, "input_pcm16_16k_mono"),
        .output_text = require_boolean_item(capabilities, "output_text"),
        .output_pcm16_16k_mono =
            require_boolean_item(capabilities, "output_pcm16_16k_mono"),
        .playback_cancel = require_boolean_item(capabilities, "playback_cancel"),
        .display_status = require_boolean_item(capabilities, "display_status"),
        .display_transcript = require_boolean_item(capabilities, "display_transcript"),
    };
}

static void validate_transcripts(
    const cJSON *pairing,
    const cJSON *authentication,
    const device_capabilities_t *capabilities)
{
    uint8_t expected[512] = {0};
    uint8_t actual[512] = {0};
    size_t actual_size = 0U;

    const size_t expected_pairing_size = decode_base64url(
        require_string_item(pairing, "associated_data"),
        expected,
        sizeof(expected),
        "pairing transcript fixture");
    require_validation(
        device_pairing_associated_data_encode(
            require_string_item(pairing, "pairing_request_id"),
            require_string_item(pairing, "installation_id"),
            require_string_item(pairing, "certificate_fingerprint"),
            require_string_item(pairing, "device_nonce"),
            require_string_item(pairing, "host_nonce"),
            capabilities,
            actual,
            sizeof(actual),
            &actual_size)
            == DEVICE_CRYPTO_RESULT_OK,
        "pairing transcript encode");
    require_validation(
        actual_size == expected_pairing_size
            && memcmp(actual, expected, actual_size) == 0,
        "pairing transcript match");

    memset(expected, 0, sizeof(expected));
    memset(actual, 0, sizeof(actual));
    const size_t expected_authentication_size = decode_base64url(
        require_string_item(authentication, "transcript"),
        expected,
        sizeof(expected),
        "authentication transcript fixture");
    require_validation(
        strcmp(require_string_item(authentication, "output_preference"), "text") == 0,
        "authentication output preference");
    require_validation(
        device_authentication_transcript_encode(
            require_string_item(authentication, "connection_id"),
            require_string_item(authentication, "host_nonce"),
            require_string_item(authentication, "device_id"),
            require_string_item(authentication, "device_nonce"),
            capabilities,
            DEVICE_OUTPUT_PREFERENCE_TEXT,
            actual,
            sizeof(actual),
            &actual_size)
            == DEVICE_CRYPTO_RESULT_OK,
        "authentication transcript encode");
    require_validation(
        actual_size == expected_authentication_size
            && memcmp(actual, expected, actual_size) == 0,
        "authentication transcript match");
}

static void validate_ed25519(const cJSON *authentication)
{
    uint8_t transcript[512] = {0};
    uint8_t seed[DEVICE_ED25519_SEED_BYTES] = {0};
    uint8_t expected_public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES] = {0};
    uint8_t expected_signature[DEVICE_ED25519_SIGNATURE_BYTES] = {0};
    uint8_t public_key[DEVICE_ED25519_PUBLIC_KEY_BYTES] = {0};
    uint8_t signature[DEVICE_ED25519_SIGNATURE_BYTES] = {0};

    const size_t transcript_size = decode_base64url(
        require_string_item(authentication, "transcript"),
        transcript,
        sizeof(transcript),
        "signature transcript fixture");
    require_validation(
        decode_base64url(
            require_string_item(authentication, "private_seed"),
            seed,
            sizeof(seed),
            "signature seed fixture")
            == sizeof(seed),
        "signature seed size");
    require_validation(
        decode_base64url(
            require_string_item(authentication, "public_key"),
            expected_public_key,
            sizeof(expected_public_key),
            "public key fixture")
            == sizeof(expected_public_key),
        "public key size");
    require_validation(
        decode_base64url(
            require_string_item(authentication, "signature"),
            expected_signature,
            sizeof(expected_signature),
            "signature fixture")
            == sizeof(expected_signature),
        "signature size");

    require_validation(
        device_ed25519_derive_public_key(seed, public_key) == DEVICE_CRYPTO_RESULT_OK
            && memcmp(public_key, expected_public_key, sizeof(public_key)) == 0,
        "public key derivation");
    require_validation(
        device_ed25519_sign(seed, transcript, transcript_size, signature)
                == DEVICE_CRYPTO_RESULT_OK
            && memcmp(signature, expected_signature, sizeof(signature)) == 0,
        "signature generation");
    require_validation(
        device_ed25519_verify(public_key, transcript, transcript_size, signature)
            == DEVICE_CRYPTO_RESULT_OK,
        "signature verification");
    signature[0] ^= 1U;
    require_validation(
        device_ed25519_verify(public_key, transcript, transcript_size, signature)
            == DEVICE_CRYPTO_RESULT_INVALID_SIGNATURE,
        "signature tamper rejection");
}

static void calculate_binding_mac(
    const uint8_t session_key[16],
    const uint8_t *label,
    size_t label_size,
    const uint8_t *transcript,
    size_t transcript_size,
    uint8_t output[32])
{
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    require_validation(sha256 != NULL, "binding hash selection");
    require_validation(label_size + transcript_size <= 256U, "binding input size");

    uint8_t inner[64U + 256U] = {0};
    uint8_t outer[64U + 32U] = {0};
    uint8_t inner_digest[32] = {0};
    memset(inner, 0x36, 64U);
    memset(outer, 0x5c, 64U);
    for (size_t index = 0U; index < 16U; ++index) {
        inner[index] ^= session_key[index];
        outer[index] ^= session_key[index];
    }
    memcpy(&inner[64U], label, label_size);
    memcpy(&inner[64U + label_size], transcript, transcript_size);
    require_validation(
        mbedtls_md(
            sha256,
            inner,
            64U + label_size + transcript_size,
            inner_digest)
            == 0,
        "binding inner hash");
    memcpy(&outer[64U], inner_digest, sizeof(inner_digest));
    require_validation(
        mbedtls_md(sha256, outer, sizeof(outer), output) == 0,
        "binding outer hash");
}

static void validate_spake2(const cJSON *pairing)
{
    deterministic_random_t random = {.sequence = 0x243f6a8885a308d3ULL};
    decode_private_scalar(
        require_string_item(pairing, "device_scalar_hex"),
        random.private_scalar);
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

    require_validation(
        decode_base64url(
            require_string_item(pairing, "device_share"),
            expected_device_share,
            sizeof(expected_device_share),
            "device share fixture")
            == sizeof(expected_device_share),
        "device share size");
    require_validation(
        decode_base64url(
            require_string_item(pairing, "host_share"),
            host_share,
            sizeof(host_share),
            "host share fixture")
            == sizeof(host_share),
        "host share size");
    const size_t associated_data_size = decode_base64url(
        require_string_item(pairing, "associated_data"),
        associated_data,
        sizeof(associated_data),
        "SPAKE2 associated data fixture");
    require_validation(
        decode_base64url(
            require_string_item(pairing, "device_confirmation"),
            expected_device_confirmation,
            sizeof(expected_device_confirmation),
            "device confirmation fixture")
            == sizeof(expected_device_confirmation),
        "device confirmation size");
    require_validation(
        decode_base64url(
            require_string_item(pairing, "host_confirmation"),
            host_confirmation,
            sizeof(host_confirmation),
            "host confirmation fixture")
            == sizeof(host_confirmation),
        "host confirmation size");
    require_validation(
        decode_base64url(
            require_string_item(pairing, "session_key"),
            session_key,
            sizeof(session_key),
            "session key fixture")
            == sizeof(session_key),
        "session key size");

    device_spake2_party_a_t *session = NULL;
    uint8_t device_share[DEVICE_SPAKE2_SHARE_BYTES] = {0};
    require_validation(
        device_spake2_party_a_begin(
            require_string_item(pairing, "pairing_code"),
            require_string_item(pairing, "pairing_request_id"),
            require_string_item(pairing, "installation_id"),
            &random_source,
            &session,
            device_share)
            == DEVICE_CRYPTO_RESULT_OK,
        "SPAKE2 begin");
    require_validation(
        session != NULL
            && memcmp(device_share, expected_device_share, sizeof(device_share)) == 0,
        "SPAKE2 device share");

    device_spake2_keys_t *keys = NULL;
    require_validation(
        device_spake2_party_a_finish(
            session,
            host_share,
            associated_data,
            associated_data_size,
            &keys)
            == DEVICE_CRYPTO_RESULT_OK,
        "SPAKE2 finish");
    require_validation(keys != NULL, "SPAKE2 keys");

    uint8_t device_confirmation[DEVICE_SPAKE2_CONFIRMATION_BYTES] = {0};
    require_validation(
        device_spake2_keys_copy_device_confirmation(keys, device_confirmation)
                == DEVICE_CRYPTO_RESULT_OK
            && memcmp(
                   device_confirmation,
                   expected_device_confirmation,
                   sizeof(device_confirmation))
                == 0,
        "SPAKE2 device confirmation");
    require_validation(
        device_spake2_keys_verify_host_confirmation(keys, host_confirmation)
            == DEVICE_CRYPTO_RESULT_OK,
        "SPAKE2 host confirmation");
    host_confirmation[0] ^= 1U;
    require_validation(
        device_spake2_keys_verify_host_confirmation(keys, host_confirmation)
            == DEVICE_CRYPTO_RESULT_CONFIRMATION_FAILED,
        "SPAKE2 confirmation tamper rejection");

    static const uint8_t label[] = "binding-check";
    static const uint8_t binding_transcript[] = "fixture-transcript";
    uint8_t expected_binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES] = {0};
    uint8_t binding_mac[DEVICE_SPAKE2_BINDING_MAC_BYTES] = {0};
    calculate_binding_mac(
        session_key,
        label,
        sizeof(label) - 1U,
        binding_transcript,
        sizeof(binding_transcript) - 1U,
        expected_binding_mac);
    require_validation(
        device_spake2_keys_binding_mac(
            keys,
            label,
            sizeof(label) - 1U,
            binding_transcript,
            sizeof(binding_transcript) - 1U,
            binding_mac)
                == DEVICE_CRYPTO_RESULT_OK
            && memcmp(binding_mac, expected_binding_mac, sizeof(binding_mac)) == 0,
        "SPAKE2 binding MAC");
    device_spake2_keys_release(keys);
    device_spake2_party_a_release(session);
}

static bool all_zero(const uint8_t *bytes, size_t size)
{
    uint8_t combined = 0U;
    for (size_t index = 0U; index < size; ++index) {
        combined |= bytes[index];
    }
    return combined == 0U;
}

static void validate_hardware_random_source(void)
{
    const device_random_source_t random_source = device_hardware_random_source();
    uint8_t first[32] = {0};
    uint8_t second[32] = {0};
    require_validation(random_source.fill != NULL, "hardware random source");
    require_validation(
        random_source.fill(random_source.context, first, sizeof(first)),
        "first hardware random sample");
    require_validation(
        random_source.fill(random_source.context, second, sizeof(second)),
        "second hardware random sample");
    require_validation(
        !all_zero(first, sizeof(first)) && !all_zero(second, sizeof(second)),
        "hardware random nonzero samples");
    require_validation(
        memcmp(first, second, sizeof(first)) != 0,
        "hardware random distinct samples");
}

static void validate_control_envelope(void)
{
    cJSON *fixture = cJSON_Parse(device_envelope_fixture_json);
    require_validation(fixture != NULL, "envelope fixture parse");
    const cJSON *valid_envelope = require_object_item(fixture, "valid_envelope");
    char *encoded = cJSON_PrintUnformatted(valid_envelope);
    require_validation(encoded != NULL, "envelope fixture encode");

    device_control_envelope_t *envelope = NULL;
    require_validation(
        device_control_envelope_decode(encoded, strlen(encoded), &envelope)
            == DEVICE_PROTOCOL_RESULT_OK,
        "control envelope decode");
    require_validation(envelope != NULL, "control envelope allocation");
    require_validation(
        device_control_envelope_protocol_minor(envelope) == 0U,
        "control envelope minor");
    require_validation(
        strcmp(
            device_control_envelope_message_id(envelope),
            require_string_item(valid_envelope, "message_id"))
            == 0,
        "control envelope message ID");
    require_validation(
        strcmp(
            device_control_envelope_message_type(envelope),
            require_string_item(valid_envelope, "type"))
            == 0,
        "control envelope message type");

    char round_trip[1024] = {0};
    size_t round_trip_size = 0U;
    require_validation(
        device_control_envelope_encode(
            device_control_envelope_message_id(envelope),
            device_control_envelope_message_type(envelope),
            device_control_envelope_payload(envelope),
            round_trip,
            sizeof(round_trip),
            &round_trip_size)
            == DEVICE_PROTOCOL_RESULT_OK,
        "control envelope encode");
    require_validation(
        round_trip_size == strlen(encoded)
            && memcmp(round_trip, encoded, round_trip_size) == 0,
        "control envelope round trip");
    device_control_envelope_release(envelope);

    static const char unsupported_major[] =
        "{\"protocol_major\":2,\"protocol_minor\":0,\"message_id\":\"m\","
        "\"type\":\"ping\",\"payload\":{}}";
    envelope = NULL;
    require_validation(
        device_control_envelope_decode(
            unsupported_major,
            sizeof(unsupported_major) - 1U,
            &envelope)
            == DEVICE_PROTOCOL_RESULT_UNSUPPORTED_PROTOCOL
            && envelope == NULL,
        "unsupported protocol rejection");

    cJSON_free(encoded);
    cJSON_Delete(fixture);
}

static uint32_t require_uint32_item(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    require_validation(
        cJSON_IsNumber(item) && item->valuedouble >= 0.0
            && item->valuedouble <= 4294967295.0
            && item->valuedouble == (double)item->valueint,
        name);
    return (uint32_t)item->valuedouble;
}

static void validate_pcm_direction(
    device_pcm_direction_t direction,
    const char *expected_header,
    uint32_t stream_id,
    uint32_t sequence)
{
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES] = {0};
    uint8_t frame[DEVICE_PCM_FRAME_BYTES] = {0};
    uint8_t expected[DEVICE_PCM_HEADER_BYTES] = {0};
    require_validation(
        device_pcm_frame_encode(
            direction,
            stream_id,
            sequence,
            payload,
            sizeof(payload),
            frame,
            sizeof(frame))
            == DEVICE_PROTOCOL_RESULT_OK,
        "PCM frame encode");
    decode_hex(expected_header, expected, sizeof(expected), "PCM fixture header");
    require_validation(
        memcmp(frame, expected, sizeof(expected)) == 0,
        "PCM fixture header match");

    device_pcm_frame_view_t view = {0};
    require_validation(
        device_pcm_frame_decode(frame, sizeof(frame), direction, &view)
                == DEVICE_PROTOCOL_RESULT_OK
            && view.direction == direction
            && view.stream_id == stream_id
            && view.sequence == sequence
            && view.payload_size == sizeof(payload),
        "PCM frame decode");
}

static void validate_pcm_frames(void)
{
    cJSON *fixture = cJSON_Parse(device_pcm_fixture_json);
    require_validation(fixture != NULL, "PCM fixture parse");
    const uint32_t stream_id = require_uint32_item(fixture, "stream_id");
    const uint32_t sequence = require_uint32_item(fixture, "sequence");
    require_validation(
        require_uint32_item(fixture, "payload_bytes") == DEVICE_PCM_PAYLOAD_BYTES,
        "PCM payload size");
    validate_pcm_direction(
        DEVICE_PCM_DIRECTION_UPLINK,
        require_string_item(fixture, "header_hex"),
        stream_id,
        sequence);
    validate_pcm_direction(
        DEVICE_PCM_DIRECTION_DOWNLINK,
        require_string_item(fixture, "downlink_header_hex"),
        stream_id,
        sequence);

    const cJSON *invalid_headers =
        cJSON_GetObjectItemCaseSensitive(fixture, "invalid_header_hex");
    require_validation(cJSON_IsArray(invalid_headers), "invalid PCM fixture headers");
    const cJSON *invalid_header = NULL;
    cJSON_ArrayForEach(invalid_header, invalid_headers) {
        require_validation(
            cJSON_IsString(invalid_header) && invalid_header->valuestring != NULL,
            "invalid PCM fixture header");
        uint8_t frame[DEVICE_PCM_FRAME_BYTES] = {0};
        decode_hex(
            invalid_header->valuestring,
            frame,
            DEVICE_PCM_HEADER_BYTES,
            "invalid PCM header encoding");
        device_pcm_frame_view_t view = {0};
        require_validation(
            device_pcm_frame_decode(
                frame,
                sizeof(frame),
                DEVICE_PCM_DIRECTION_UPLINK,
                &view)
                == DEVICE_PROTOCOL_RESULT_INVALID_PCM_FRAME,
            "invalid PCM header rejection");
    }
    cJSON_Delete(fixture);
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting target protocol cryptography validation");
    cJSON *fixture = cJSON_Parse(device_crypto_fixture_json);
    require_validation(fixture != NULL, "fixture parse");
    const cJSON *pairing = require_object_item(fixture, "pairing");
    const cJSON *authentication = require_object_item(fixture, "authentication");
    const device_capabilities_t capabilities = parse_capabilities(pairing);

    validate_control_envelope();
    validate_pcm_frames();
    validate_transcripts(pairing, authentication, &capabilities);
    validate_ed25519(authentication);
    validate_spake2(pairing);
    validate_hardware_random_source();

    cJSON_Delete(fixture);
    ESP_LOGI(TAG, "target protocol cryptography validation passed");
}
