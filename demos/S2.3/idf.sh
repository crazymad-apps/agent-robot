#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_VERSION="v6.0.2"
TARGET="esp32s3"

cd "$PROJECT_DIR"

usage() {
    cat <<'EOF'
用法：
  ./idf.sh check
  ./idf.sh set-target
  ./idf.sh build
  ./idf.sh ports
  ./idf.sh flash <串口>
  ./idf.sh monitor <串口>
  ./idf.sh flash-monitor <串口>
  ./idf.sh menuconfig
  ./idf.sh size
  ./idf.sh fullclean

示例：
  ./idf.sh flash-monitor /dev/cu.usbserial-110
EOF
}

require_eim() {
    if ! command -v eim >/dev/null 2>&1; then
        echo "错误：未找到 eim，请先安装并配置 EIM。" >&2
        exit 1
    fi
}

run_idf() {
    local command="$1"
    require_eim
    echo "+ eim run \"$command\" $IDF_VERSION"
    eim run "$command" "$IDF_VERSION"
}

ensure_target() {
    if [[ ! -f sdkconfig ]] || ! grep -q '^CONFIG_IDF_TARGET="esp32s3"$' sdkconfig; then
        echo "首次构建：设置目标芯片为 $TARGET"
        run_idf "idf.py set-target $TARGET"
    fi
}

require_port() {
    local port="${1:-}"
    if [[ -z "$port" ]]; then
        echo "错误：缺少串口参数。先运行 ./idf.sh ports 查看可用串口。" >&2
        exit 1
    fi
    if [[ ! "$port" =~ ^/dev/(cu|tty)\.[A-Za-z0-9._-]+$ ]]; then
        echo "错误：串口路径格式不合法：$port" >&2
        exit 1
    fi
    if [[ ! -e "$port" ]]; then
        echo "错误：串口不存在：$port" >&2
        exit 1
    fi
}

command_name="${1:-help}"
case "$command_name" in
    check)
        require_eim
        eim --version
        eim list
        run_idf "idf.py --version"
        run_idf "ninja --version"
        ;;
    set-target)
        run_idf "idf.py set-target $TARGET"
        ;;
    build)
        ensure_target
        run_idf "idf.py build"
        ;;
    ports)
        echo "macOS 当前串口："
        found=0
        for port in /dev/cu.*; do
            if [[ -e "$port" ]]; then
                echo "  $port"
                found=1
            fi
        done
        if [[ "$found" -eq 0 ]]; then
            echo "  未发现串口。连接开发板后重试。"
        fi
        ;;
    flash)
        require_port "${2:-}"
        ensure_target
        run_idf "idf.py -p ${2} flash"
        ;;
    monitor)
        require_port "${2:-}"
        ensure_target
        run_idf "idf.py -p ${2} monitor"
        ;;
    flash-monitor)
        require_port "${2:-}"
        ensure_target
        run_idf "idf.py -p ${2} flash monitor"
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
        echo "错误：未知命令：$command_name" >&2
        usage >&2
        exit 1
        ;;
esac
