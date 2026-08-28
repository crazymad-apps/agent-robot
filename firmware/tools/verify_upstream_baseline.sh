#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE="$FIRMWARE_DIR/compatibility/device-protocol-v1.json"
FIXTURE_DIR="$FIRMWARE_DIR/test/upstream/device-protocol-v1"
UPSTREAM_REPOSITORY="$(jq -r '.ez_assistant.repository_path' "$BASELINE")"

verify_file_hash() {
    local file_path="$1"
    local expected_hash="$2"
    if [[ ! -f "$file_path" ]]; then
        echo "Baseline file is missing: $file_path" >&2
        exit 1
    fi
    local actual_hash
    actual_hash="$(shasum -a 256 "$file_path" | awk '{print $1}')"
    if [[ "$actual_hash" != "$expected_hash" ]]; then
        echo "Baseline hash mismatch: $file_path" >&2
        echo "expected=$expected_hash" >&2
        echo "actual=$actual_hash" >&2
        exit 1
    fi
}

while IFS=$'\t' read -r fixture_name expected_hash; do
    verify_file_hash "$FIXTURE_DIR/$fixture_name" "$expected_hash"
done < <(jq -r '.fixtures | to_entries[] | [.key, .value] | @tsv' "$BASELINE")

while IFS=$'\t' read -r relative_path expected_hash; do
    verify_file_hash "$UPSTREAM_REPOSITORY/$relative_path" "$expected_hash"
done < <(jq -r '.ez_assistant.relevant_source_sha256 | to_entries[] | [.key, .value] | @tsv' "$BASELINE")

echo "Device protocol upstream baseline passed."
