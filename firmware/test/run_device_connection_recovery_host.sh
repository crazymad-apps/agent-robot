#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VALIDATION_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agent-robot-device-recovery.XXXXXX")"

cleanup() {
    rm -rf "$VALIDATION_DIR"
}
trap cleanup EXIT

if ! command -v node >/dev/null 2>&1; then
    echo "Node.js is required to run the Device Connection recovery Host." >&2
    exit 1
fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "OpenSSL is required to create the ephemeral validation certificate." >&2
    exit 1
fi
if [[ ! -x /usr/bin/dns-sd ]]; then
    echo "macOS dns-sd is required to advertise the recovery Host." >&2
    exit 1
fi

openssl req \
    -x509 \
    -newkey rsa:2048 \
    -nodes \
    -days 1 \
    -subj "/CN=agent-robot-device-connection-recovery" \
    -keyout "$VALIDATION_DIR/private-key.pem" \
    -out "$VALIDATION_DIR/certificate.pem" \
    >/dev/null 2>&1

node "$FIRMWARE_DIR/test/device_connection_recovery_host.mjs" \
    --certificate "$VALIDATION_DIR/certificate.pem" \
    --private-key "$VALIDATION_DIR/private-key.pem"
