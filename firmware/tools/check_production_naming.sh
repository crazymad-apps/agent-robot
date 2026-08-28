#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRODUCTION_PATHS=(
    "$FIRMWARE_DIR/main"
    "$FIRMWARE_DIR/components"
    "$FIRMWARE_DIR/CMakeLists.txt"
    "$FIRMWARE_DIR/sdkconfig.defaults"
    "$FIRMWARE_DIR/partitions.csv"
)
PROHIBITED_PATTERN='demo|temporary|quick|hack|mock|fake|test_mode|s3[._-]?1'

existing_paths=()
for production_path in "${PRODUCTION_PATHS[@]}"; do
    [[ -e "$production_path" ]] && existing_paths+=("$production_path")
done

if rg --hidden --ignore-case --line-number \
    --glob '!**/device_crypto/third_party/monocypher/**' \
    "$PROHIBITED_PATTERN" "${existing_paths[@]}"; then
    echo "Production naming policy violation detected." >&2
    exit 1
fi

if ! grep -q '^project(agent_robot_terminal)$' "$FIRMWARE_DIR/CMakeLists.txt"; then
    echo "ESP-IDF project name must remain agent_robot_terminal." >&2
    exit 1
fi

echo "Production naming policy passed."
