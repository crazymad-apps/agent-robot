#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE="$FIRMWARE_DIR/test/upstream/device-protocol-v1/pcm-v1.json"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agent-robot-host-tests.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

"$FIRMWARE_DIR/tools/check_third_party_sources.sh"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    -I"$FIRMWARE_DIR/components/device_crypto/third_party/monocypher" \
    "$FIRMWARE_DIR/components/device_crypto/device_base64url.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_ed25519.c" \
    "$FIRMWARE_DIR/components/device_crypto/third_party/monocypher/monocypher.c" \
    "$FIRMWARE_DIR/components/device_crypto/third_party/monocypher/monocypher-ed25519.c" \
    "$FIRMWARE_DIR/test/host/device_ed25519_test.c" \
    -o "$BUILD_DIR/device_ed25519_test"

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
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    "$FIRMWARE_DIR/components/device_protocol/device_message_replay_window.c" \
    "$FIRMWARE_DIR/test/host/device_message_replay_window_test.c" \
    -o "$BUILD_DIR/device_message_replay_window_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/host_discovery/include" \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    "$FIRMWARE_DIR/components/host_discovery/host_selection.c" \
    "$FIRMWARE_DIR/test/host/host_discovery_selection_test.c" \
    -o "$BUILD_DIR/host_discovery_selection_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/device_connection/include" \
    "$FIRMWARE_DIR/components/device_connection/device_connection_supervisor.c" \
    "$FIRMWARE_DIR/test/host/device_connection_supervisor_test.c" \
    -o "$BUILD_DIR/device_connection_supervisor_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/network_connectivity/private_include" \
    "$FIRMWARE_DIR/components/network_connectivity/network_reconnect_policy.c" \
    "$FIRMWARE_DIR/test/host/network_reconnect_policy_test.c" \
    -o "$BUILD_DIR/network_reconnect_policy_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    "$FIRMWARE_DIR/components/device_crypto/device_base64url.c" \
    "$FIRMWARE_DIR/test/host/device_base64url_test.c" \
    -o "$BUILD_DIR/device_base64url_test"

CJSON_DIR="$FIRMWARE_DIR/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$CJSON_DIR/cJSON.c" ]]; then
    echo "Managed cJSON source is unavailable; run ./idf.sh build before host tests." >&2
    exit 1
fi
cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -DCJSON_NESTING_LIMIT=64 \
    -DCJSON_CIRCULAR_LIMIT=1000 \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    -I"$CJSON_DIR" \
    "$CJSON_DIR/cJSON.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_control_envelope.c" \
    "$FIRMWARE_DIR/test/host/device_control_envelope_test.c" \
    -lm \
    -o "$BUILD_DIR/device_control_envelope_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -DCJSON_NESTING_LIMIT=64 \
    -DCJSON_CIRCULAR_LIMIT=1000 \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    -I"$CJSON_DIR" \
    "$CJSON_DIR/cJSON.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_control_envelope.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_application_heartbeat.c" \
    "$FIRMWARE_DIR/test/host/device_application_heartbeat_test.c" \
    -lm \
    -o "$BUILD_DIR/device_application_heartbeat_test"

"$BUILD_DIR/device_application_heartbeat_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -DCJSON_NESTING_LIMIT=64 \
    -DCJSON_CIRCULAR_LIMIT=1000 \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    -I"$CJSON_DIR" \
    "$CJSON_DIR/cJSON.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_control_envelope.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_media_control.c" \
    "$FIRMWARE_DIR/test/host/device_media_control_test.c" \
    -lm \
    -o "$BUILD_DIR/device_media_control_test"

"$BUILD_DIR/device_media_control_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    -I"$FIRMWARE_DIR/components/utterance_cache/include" \
    -I"$FIRMWARE_DIR/components/utterance_cache/private_include" \
    "$FIRMWARE_DIR/components/utterance_cache/utterance_cache.c" \
    "$FIRMWARE_DIR/test/host/utterance_cache_test.c" \
    -o "$BUILD_DIR/utterance_cache_test"

"$BUILD_DIR/utterance_cache_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/audio_io/include" \
    "$FIRMWARE_DIR/components/audio_io/audio_pcm_conversion.c" \
    "$FIRMWARE_DIR/test/host/audio_pcm_conversion_test.c" \
    -o "$BUILD_DIR/audio_pcm_conversion_test"

"$BUILD_DIR/audio_pcm_conversion_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/audio_io/private_include" \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    -I"$CJSON_DIR" \
    "$FIRMWARE_DIR/components/audio_io/audio_playback_queue.c" \
    "$FIRMWARE_DIR/test/host/audio_playback_queue_test.c" \
    -o "$BUILD_DIR/audio_playback_queue_test"

"$BUILD_DIR/audio_playback_queue_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    "$FIRMWARE_DIR/components/device_crypto/device_base64url.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_transcript.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_capabilities.c" \
    "$FIRMWARE_DIR/test/host/device_transcript_test.c" \
    -o "$BUILD_DIR/device_transcript_test"

if ! pkg-config --exists mbedcrypto; then
    echo "mbedcrypto development package is required for SPAKE2 host tests." >&2
    exit 1
fi
MBEDCRYPTO_CFLAGS="$(pkg-config --cflags mbedcrypto)"
MBEDCRYPTO_LIBS="$(pkg-config --libs mbedcrypto)"
cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    $MBEDCRYPTO_CFLAGS \
    -I"$FIRMWARE_DIR/components/wss_transport/private_include" \
    "$FIRMWARE_DIR/components/wss_transport/wss_frame_codec.c" \
    "$FIRMWARE_DIR/components/wss_transport/wss_upgrade.c" \
    "$FIRMWARE_DIR/test/host/wss_transport_codec_test.c" \
    $MBEDCRYPTO_LIBS \
    -o "$BUILD_DIR/wss_transport_codec_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    $MBEDCRYPTO_CFLAGS \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    "$FIRMWARE_DIR/components/device_crypto/device_base64url.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_spake2.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_transcript.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_capabilities.c" \
    "$FIRMWARE_DIR/test/host/device_spake2_test.c" \
    $MBEDCRYPTO_LIBS \
    -o "$BUILD_DIR/device_spake2_test"

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
"$BUILD_DIR/device_message_replay_window_test"
"$BUILD_DIR/host_discovery_selection_test"
"$BUILD_DIR/device_connection_supervisor_test"
"$BUILD_DIR/network_reconnect_policy_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/interaction/private_include" \
    "$FIRMWARE_DIR/components/interaction/terminal_button_policy.c" \
    "$FIRMWARE_DIR/test/host/terminal_button_policy_test.c" \
    -o "$BUILD_DIR/terminal_button_policy_test"

"$BUILD_DIR/terminal_button_policy_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/interaction/private_include" \
    "$FIRMWARE_DIR/components/interaction/terminal_interaction_policy.c" \
    "$FIRMWARE_DIR/test/host/terminal_interaction_policy_test.c" \
    -o "$BUILD_DIR/terminal_interaction_policy_test"

"$BUILD_DIR/terminal_interaction_policy_test"

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

"$BUILD_DIR/device_ed25519_test" \
    "$(jq -r '.authentication.transcript' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.private_seed' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.public_key' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.signature' "$CRYPTO_FIXTURE")"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$FIRMWARE_DIR/components/device_identity/include" \
    -I"$FIRMWARE_DIR/components/device_identity/private_include" \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    -I"$FIRMWARE_DIR/components/device_crypto/third_party/monocypher" \
    -I"$FIRMWARE_DIR/components/host_discovery/include" \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    "$FIRMWARE_DIR/components/device_identity/device_identity_record.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_ed25519.c" \
    "$FIRMWARE_DIR/components/device_crypto/third_party/monocypher/monocypher.c" \
    "$FIRMWARE_DIR/components/device_crypto/third_party/monocypher/monocypher-ed25519.c" \
    "$FIRMWARE_DIR/test/host/device_identity_record_test.c" \
    -o "$BUILD_DIR/device_identity_record_test"

"$BUILD_DIR/device_identity_record_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -DCJSON_NESTING_LIMIT=64 \
    -DCJSON_CIRCULAR_LIMIT=1000 \
    -I"$FIRMWARE_DIR/components/device_authentication/include" \
    -I"$FIRMWARE_DIR/components/device_identity/include" \
    -I"$FIRMWARE_DIR/components/device_identity/private_include" \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    -I"$FIRMWARE_DIR/components/device_crypto/third_party/monocypher" \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    -I"$FIRMWARE_DIR/components/host_discovery/include" \
    -I"$CJSON_DIR" \
    "$CJSON_DIR/cJSON.c" \
    "$FIRMWARE_DIR/components/device_authentication/device_authentication.c" \
    "$FIRMWARE_DIR/components/device_identity/device_identity_record.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_base64url.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_ed25519.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_transcript.c" \
    "$FIRMWARE_DIR/components/device_crypto/third_party/monocypher/monocypher.c" \
    "$FIRMWARE_DIR/components/device_crypto/third_party/monocypher/monocypher-ed25519.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_capabilities.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_control_envelope.c" \
    "$FIRMWARE_DIR/test/host/device_authentication_test.c" \
    -lm \
    -o "$BUILD_DIR/device_authentication_test"

"$BUILD_DIR/device_authentication_test" \
    "$(jq -r '.authentication.private_seed' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.public_key' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.signature' "$CRYPTO_FIXTURE")"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -DCJSON_NESTING_LIMIT=64 \
    -DCJSON_CIRCULAR_LIMIT=1000 \
    -I"$FIRMWARE_DIR/components/device_pairing/include" \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    -I"$CJSON_DIR" \
    "$CJSON_DIR/cJSON.c" \
    "$FIRMWARE_DIR/components/device_pairing/device_pairing_protocol.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_base64url.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_control_envelope.c" \
    "$FIRMWARE_DIR/test/host/device_pairing_protocol_test.c" \
    -lm \
    -o "$BUILD_DIR/device_pairing_protocol_test"

"$BUILD_DIR/device_pairing_protocol_test"

cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Werror \
    -D_POSIX_C_SOURCE=200809L \
    -DCJSON_NESTING_LIMIT=64 \
    -DCJSON_CIRCULAR_LIMIT=1000 \
    $MBEDCRYPTO_CFLAGS \
    -I"$FIRMWARE_DIR/components/device_pairing/include" \
    -I"$FIRMWARE_DIR/components/device_identity/include" \
    -I"$FIRMWARE_DIR/components/device_crypto/include" \
    -I"$FIRMWARE_DIR/components/device_crypto/third_party/monocypher" \
    -I"$FIRMWARE_DIR/components/device_protocol/include" \
    -I"$FIRMWARE_DIR/components/host_discovery/include" \
    -I"$CJSON_DIR" \
    "$CJSON_DIR/cJSON.c" \
    "$FIRMWARE_DIR/components/device_pairing/device_pairing.c" \
    "$FIRMWARE_DIR/components/device_pairing/device_pairing_protocol.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_base64url.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_ed25519.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_spake2.c" \
    "$FIRMWARE_DIR/components/device_crypto/device_transcript.c" \
    "$FIRMWARE_DIR/components/device_crypto/third_party/monocypher/monocypher.c" \
    "$FIRMWARE_DIR/components/device_crypto/third_party/monocypher/monocypher-ed25519.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_capabilities.c" \
    "$FIRMWARE_DIR/components/device_protocol/device_control_envelope.c" \
    "$FIRMWARE_DIR/components/host_discovery/host_selection.c" \
    "$FIRMWARE_DIR/test/host/device_pairing_state_test.c" \
    $MBEDCRYPTO_LIBS \
    -lm \
    -o "$BUILD_DIR/device_pairing_state_test"

"$BUILD_DIR/device_pairing_state_test"

ENVELOPE_FIXTURE="$FIRMWARE_DIR/test/upstream/device-protocol-v1/envelope-v1.json"
"$BUILD_DIR/device_control_envelope_test" "$(jq -c '.valid_envelope' "$ENVELOPE_FIXTURE")"

CAPABILITY_BITS="$(jq -r '
    .pairing.capabilities
    | [.input_text, .input_pcm16_16k_mono, .output_text, .output_pcm16_16k_mono,
       .playback_cancel, .display_status, .display_transcript]
    | map(if . then "1" else "0" end)
    | join("")
' "$CRYPTO_FIXTURE")"
"$BUILD_DIR/device_transcript_test" \
    "$(jq -r '.pairing.associated_data' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.pairing_request_id' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.installation_id' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.certificate_fingerprint' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.device_nonce' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.host_nonce' "$CRYPTO_FIXTURE")" \
    "$CAPABILITY_BITS" \
    "$(jq -r '.authentication.transcript' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.connection_id' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.host_nonce' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.device_id' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.device_nonce' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.authentication.output_preference' "$CRYPTO_FIXTURE")"

"$BUILD_DIR/device_spake2_test" \
    "$(jq -r '.pairing.pairing_code' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.pairing_request_id' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.installation_id' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.device_scalar_hex' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.device_share' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.host_share' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.associated_data' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.device_confirmation' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.host_confirmation' "$CRYPTO_FIXTURE")" \
    "$(jq -r '.pairing.session_key' "$CRYPTO_FIXTURE")"

"$BUILD_DIR/wss_transport_codec_test"
