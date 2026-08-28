#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_VERSION="v6.0.2"
TARGET="esp32s3"

cd "$PROJECT_DIR"

usage() {
    printf '%s\n' \
        "用法：" \
        "  ./idf.sh check" \
        "  ./idf.sh build" \
        "  ./idf.sh ports" \
        "  ./idf.sh flash <串口>" \
        "  ./idf.sh monitor <串口>" \
        "  ./idf.sh flash-monitor <串口>" \
        "  ./idf.sh menuconfig" \
        "  ./idf.sh size" \
        "  ./idf.sh fullclean"
}

run_idf() {
    local command="$1"
    if ! command -v eim >/dev/null 2>&1; then
        echo "错误：未找到 eim。" >&2
        exit 1
    fi
    echo "+ eim run \"$command\" $IDF_VERSION"
    eim run "$command" "$IDF_VERSION"
}

ensure_target() {
    if [[ ! -f sdkconfig ]] || ! grep -q '^CONFIG_IDF_TARGET="esp32s3"$' sdkconfig; then
        run_idf "idf.py set-target $TARGET"
    fi
}

require_port() {
    local port="${1:-}"
    if [[ ! "$port" =~ ^/dev/(cu|tty)\.[A-Za-z0-9._-]+$ ]] || [[ ! -e "$port" ]]; then
        echo "错误：串口不存在或格式不合法：$port" >&2
        exit 1
    fi
}

case "${1:-help}" in
    check)
        run_idf "idf.py --version"
        run_idf "ninja --version"
        ;;
    build)
        ensure_target
        run_idf "idf.py build"
        ;;
    ports)
        for port in /dev/cu.*; do
            [[ -e "$port" ]] && echo "$port"
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
