#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "[1/4] 检查脚本语法"
bash -n idf.sh test.sh

echo "[2/4] 使用 ESP-IDF v6.0.2 构建"
./idf.sh build

echo "[3/4] 检查烧录产物"
artifacts=(
    "build/agent_robot_demo.bin"
    "build/bootloader/bootloader.bin"
    "build/partition_table/partition-table.bin"
    "build/srmodels/srmodels.bin"
)

for artifact in "${artifacts[@]}"; do
    if [[ ! -s "$artifact" ]]; then
        echo "失败：缺少产物或文件为空：$artifact" >&2
        exit 1
    fi
    echo "  PASS $artifact"
done

echo "[4/4] 检查 S2 冻结配置"
grep -q '^CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y$' sdkconfig
grep -q '^model,.*0x250000, 0x100000,' partitions.csv
grep -q '^  espressif/esp-sr:$' main/idf_component.yml
grep -q '^    version: "==2.4.7"$' main/idf_component.yml

echo "PASS：脚本语法、ESP-IDF 构建、烧录产物及 S2 冻结配置检查均通过。"
