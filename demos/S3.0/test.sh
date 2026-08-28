#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

bash -n idf.sh test.sh
./idf.sh build

test -s build/agent_robot_s3_demo.bin
test -s build/bootloader/bootloader.bin
test -s build/partition_table/partition-table.bin
grep -q '^  espressif/esp_websocket_client:$' main/idf_component.yml
grep -q '^    version: "==1.7.0"$' main/idf_component.yml
grep -q '^    version: 1.7.0$' dependencies.lock

echo "PASS：S3.0 固件构建、烧录产物和 WebSocket 依赖检查通过。"
