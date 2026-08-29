#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="$FIRMWARE_DIR/test/target_transport"
IDF_VERSION="v6.0.2"
IDF_TARGET="esp32s3"

usage() {
    printf '%s\n' \
        "Usage:" \
        "  ./test/run_target_transport_validation.sh build" \
        "  ./test/run_target_transport_validation.sh ports" \
        "  ./test/run_target_transport_validation.sh flash-monitor <serial-port>" \
        "  ./test/run_target_transport_validation.sh fullclean"
}

run_idf() {
    local command="$1"
    if ! command -v eim >/dev/null 2>&1; then
        echo "eim is required to select the pinned ESP-IDF runtime." >&2
        exit 1
    fi
    (cd "$PROJECT_DIR" && eim run "$command" "$IDF_VERSION")
}

ensure_target() {
    if [[ ! -f "$PROJECT_DIR/sdkconfig" ]] \
        || ! grep -q '^CONFIG_IDF_TARGET="esp32s3"$' "$PROJECT_DIR/sdkconfig"; then
        run_idf "idf.py set-target $IDF_TARGET"
    fi
}

require_port() {
    local serial_port="${1:-}"
    if [[ ! "$serial_port" =~ ^/dev/(cu|tty)\.[A-Za-z0-9._-]+$ ]] \
        || [[ ! -e "$serial_port" ]]; then
        echo "Serial port does not exist or is not a supported device path: $serial_port" >&2
        exit 1
    fi
}

case "${1:-help}" in
    build)
        "$FIRMWARE_DIR/tools/check_production_naming.sh"
        "$FIRMWARE_DIR/tools/check_third_party_sources.sh"
        ensure_target
        run_idf "idf.py build"
        ;;
    ports)
        for serial_port in /dev/cu.*; do
            [[ -e "$serial_port" ]] && echo "$serial_port"
        done
        ;;
    flash-monitor)
        require_port "${2:-}"
        ensure_target
        run_idf "idf.py -p ${2} flash monitor"
        ;;
    fullclean)
        run_idf "idf.py fullclean"
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
