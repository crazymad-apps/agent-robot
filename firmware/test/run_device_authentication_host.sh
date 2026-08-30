#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VALIDATION_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agent-robot-device-authentication.XXXXXX")"

cleanup() {
    rm -rf "$VALIDATION_DIR"
}
trap cleanup EXIT

for command_name in node openssl; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "$command_name is required to run the Device authentication Host." >&2
        exit 1
    fi
done
if [[ ! -x /usr/bin/dns-sd ]]; then
    echo "macOS dns-sd is required to advertise the authentication Host." >&2
    exit 1
fi

openssl req \
    -x509 \
    -newkey rsa:2048 \
    -nodes \
    -days 1 \
    -subj "/CN=agent-robot-device-authentication" \
    -keyout "$VALIDATION_DIR/private-key.pem" \
    -out "$VALIDATION_DIR/certificate.pem" \
    >/dev/null 2>&1

node "$FIRMWARE_DIR/test/device_authentication_host.mjs" \
    --certificate "$VALIDATION_DIR/certificate.pem" \
    --private-key "$VALIDATION_DIR/private-key.pem"
