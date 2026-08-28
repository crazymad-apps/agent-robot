#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_VERSION="v6.0.2"
IDF_TARGET="esp32s3"

cd "$FIRMWARE_DIR"

usage() {
    printf '%s\n' \
        "Usage:" \
        "  ./idf.sh check" \
        "  ./idf.sh build" \
        "  ./idf.sh ports" \
        "  ./idf.sh flash <serial-port>" \
        "  ./idf.sh monitor <serial-port>" \
        "  ./idf.sh flash-monitor <serial-port>" \
        "  ./idf.sh menuconfig" \
        "  ./idf.sh size" \
        "  ./idf.sh fullclean"
}

run_idf() {
    local command="$1"
    if ! command -v eim >/dev/null 2>&1; then
        echo "eim is required to select the pinned ESP-IDF runtime." >&2
        exit 1
    fi
    eim run "$command" "$IDF_VERSION"
}

ensure_target() {
    if [[ ! -f sdkconfig ]] || ! grep -q '^CONFIG_IDF_TARGET="esp32s3"$' sdkconfig; then
        run_idf "idf.py set-target $IDF_TARGET"
    fi
}

require_port() {
    local serial_port="${1:-}"
    if [[ ! "$serial_port" =~ ^/dev/(cu|tty)\.[A-Za-z0-9._-]+$ ]] || [[ ! -e "$serial_port" ]]; then
        echo "Serial port does not exist or is not a supported device path: $serial_port" >&2
        exit 1
    fi
}

case "${1:-help}" in
    check)
        "$FIRMWARE_DIR/tools/check_production_naming.sh"
        run_idf "idf.py --version"
        run_idf "ninja --version"
        ;;
    build)
        "$FIRMWARE_DIR/tools/check_production_naming.sh"
        ensure_target
        run_idf "idf.py build"
        ;;
    ports)
        for serial_port in /dev/cu.*; do
            [[ -e "$serial_port" ]] && echo "$serial_port"
        done
        ;;
    flash|monitor|flash-monitor)
        require_port "${2:-}"
        ensure_target
        if [[ "$1" == "flash-monitor" ]]; then
            run_idf "idf.py -p ${2} flash monitor"
        else
            run_idf "idf.py -p ${2} $1"
        fi
        ;;
    menuconfig)
        ensure_target
        run_idf "idf.py menuconfig"
        ;;
    size)
        ensure_target
        run_idf "idf.py size-components"
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
