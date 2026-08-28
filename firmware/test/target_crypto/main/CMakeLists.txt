set(CRYPTO_FIXTURE
    "${CMAKE_CURRENT_LIST_DIR}/../../upstream/device-protocol-v1/crypto-v1.json")
set(ENVELOPE_FIXTURE
    "${CMAKE_CURRENT_LIST_DIR}/../../upstream/device-protocol-v1/envelope-v1.json")
set(PCM_FIXTURE
    "${CMAKE_CURRENT_LIST_DIR}/../../upstream/device-protocol-v1/pcm-v1.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CRYPTO_FIXTURE}"
    "${ENVELOPE_FIXTURE}"
    "${PCM_FIXTURE}"
)

file(READ "${CRYPTO_FIXTURE}" CRYPTO_FIXTURE_HEX HEX)
file(READ "${ENVELOPE_FIXTURE}" ENVELOPE_FIXTURE_HEX HEX)
file(READ "${PCM_FIXTURE}" PCM_FIXTURE_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CRYPTO_FIXTURE_BYTES
    "${CRYPTO_FIXTURE_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ENVELOPE_FIXTURE_BYTES
    "${ENVELOPE_FIXTURE_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," PCM_FIXTURE_BYTES
    "${PCM_FIXTURE_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/device_crypto_fixture_data.h"
    "#pragma once\n"
    "static const char device_crypto_fixture_json[] = {"
    "${CRYPTO_FIXTURE_BYTES}"
    "0x00};\n"
    "static const char device_envelope_fixture_json[] = {"
    "${ENVELOPE_FIXTURE_BYTES}"
    "0x00};\n"
    "static const char device_pcm_fixture_json[] = {"
    "${PCM_FIXTURE_BYTES}"
    "0x00};\n")

idf_component_register(
    SRCS "target_crypto_validation.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES device_crypto device_protocol espressif__cjson mbedtls
)
