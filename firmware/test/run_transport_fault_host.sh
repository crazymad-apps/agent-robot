#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VALIDATION_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agent-robot-transport-fault.XXXXXX")"

cleanup() {
    rm -rf "$VALIDATION_DIR"
}
trap cleanup EXIT

if ! command -v node >/dev/null 2>&1; then
    echo "Node.js is required for the transport fault Host." >&2
    exit 1
fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "OpenSSL is required to create the ephemeral validation certificate." >&2
    exit 1
fi
if [[ ! -x /usr/bin/dns-sd ]]; then
    echo "macOS dns-sd is required to advertise the validation Host." >&2
    exit 1
fi

openssl req \
    -x509 \
    -newkey rsa:2048 \
    -nodes \
    -days 1 \
    -subj "/CN=agent-robot-transport-validation" \
    -keyout "$VALIDATION_DIR/private-key.pem" \
    -out "$VALIDATION_DIR/certificate.pem" \
    >/dev/null 2>&1

node "$FIRMWARE_DIR/test/transport_fault_host.mjs" \
    --certificate "$VALIDATION_DIR/certificate.pem" \
    --private-key "$VALIDATION_DIR/private-key.pem"
