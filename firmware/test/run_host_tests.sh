#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE="$FIRMWARE_DIR/test/upstream/device-protocol-v1/pcm-v1.json"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agent-robot-host-tests.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    "$FIRMWARE_DIR/components/device_protocol/device_pcm_frame.c" \
    "$FIRMWARE_DIR/test/host/device_pcm_frame_test.c" \
    -o "$BUILD_DIR/device_pcm_frame_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    "$FIRMWARE_DIR/components/device_crypto/device_base64url.c" \
    "$FIRMWARE_DIR/test/host/device_base64url_test.c" \
    -o "$BUILD_DIR/device_base64url_test"

arguments=(
    "$(jq -r '.header_hex' "$FIXTURE")"
    "$(jq -r '.downlink_header_hex' "$FIXTURE")"
    "$(jq -r '.stream_id' "$FIXTURE")"
    "$(jq -r '.sequence' "$FIXTURE")"
    "$(jq -r '.payload_bytes' "$FIXTURE")"
)
while IFS= read -r invalid_header; do
    arguments+=("$invalid_header")
done < <(jq -r '.invalid_header_hex[]' "$FIXTURE")

"$BUILD_DIR/device_pcm_frame_test" "${arguments[@]}"

CRYPTO_FIXTURE="$FIRMWARE_DIR/test/upstream/device-protocol-v1/crypto-v1.json"
"$BUILD_DIR/device_base64url_test" \
    "$(jq -r '.pairing.associated_data' "$CRYPTO_FIXTURE")" 235 \
    "$(jq -r '.pairing.device_share' "$CRYPTO_FIXTURE")" 65 \
    "$(jq -r '.pairing.host_share' "$CRYPTO_FIXTURE")" 65 \
    "$(jq -r '.pairing.device_confirmation' "$CRYPTO_FIXTURE")" 32 \
    "$(jq -r '.pairing.host_confirmation' "$CRYPTO_FIXTURE")" 32 \
    "$(jq -r '.pairing.session_key' "$CRYPTO_FIXTURE")" 16 \
    "$(jq -r '.authentication.transcript' "$CRYPTO_FIXTURE")" 171 \
    "$(jq -r '.authentication.private_seed' "$CRYPTO_FIXTURE")" 32 \
    "$(jq -r '.authentication.public_key' "$CRYPTO_FIXTURE")" 32 \
    "$(jq -r '.authentication.signature' "$CRYPTO_FIXTURE")" 64
