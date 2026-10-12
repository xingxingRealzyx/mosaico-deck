#!/usr/bin/env bash
# =============================================================================
#  flash.sh —— ESP-Mosaico 免按键烧录（一条命令，全程不用碰 BOOT）
#
#  用法:
#      ./flash.sh                          # 默认 /dev/cu.usbmodem21201
#      ./flash.sh /dev/cu.usbmodem12301    # 指定端口
#      PORT=/dev/cu.usbmodem12301 ./flash.sh
#
#  流程:
#      1. reset-to-download.sh —— 用 DTR/RTS 时序让固件把芯片切进 ROM 下载模式
#      2. 等 ROM 的 USB 重新枚举（应用态和 ROM 态的描述符一致，端口名不变）
#      3. 芯片已经在下载模式了，用 --before no-reset 直接烧
#
#  为什么不能一步 `idf.py flash` 搞定
#  --------------------------------
#  S31 rev0 的 ROM chip_usb_dw_prepare_persist 是空 stub，USB 不能跨复位存活。
#  esptool 的 --before default-reset 是"先复位、再在同一个已打开的串口 fd 上
#  重连"，而 OTG 从应用态切到 ROM 态时 USB 会重新枚举，pyserial 手里那个 fd
#  就作废了，报 `Could not configure port: (6, 'Device not configured')`。
#  （USB-Serial-JTAG 没这个问题 —— 它由硬件实现、复位不断连接 —— 但 Mosaico
#  的 Type-C 接的是 USB-OTG。）见 esp-idf issue #18992；官方也是让应用层监听
#  DTR/RTS 做"软自动下载"。
#
#  前置条件: 先 `idf.py build` 成功（脚本要读 build/flash_args）。
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$HERE/build"
DEFAULT_PORT="/dev/cu.usbmodem21201"

if [ -n "${1:-}" ]; then
    case "$1" in
        -h|--help)
            sed -n '4,11p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
    esac
fi

# ---- 端口 -------------------------------------------------------------------
# 优先级：命令行参数 > 环境变量 PORT > 默认端口。
if [ -n "${1:-}" ]; then
    PORT="$1"
    PORT_IS_DEFAULT=0
elif [ -n "${PORT:-}" ]; then
    PORT_IS_DEFAULT=0
else
    PORT="$DEFAULT_PORT"
    PORT_IS_DEFAULT=1
fi

if [ ! -e "$PORT" ] && [ "$PORT_IS_DEFAULT" -eq 1 ]; then
    # 只有"没显式指定端口"时才自动找一个，免得把别人的板子刷了。
    FOUND="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
    if [ -n "$FOUND" ]; then
        echo "提示: 默认端口 $DEFAULT_PORT 不存在，改用 $FOUND"
        PORT="$FOUND"
    fi
fi
if [ ! -e "$PORT" ]; then
    echo "错误: 端口 $PORT 不存在。" >&2
    echo "      当前可用: $(ls /dev/cu.usbmodem* 2>/dev/null || echo '(无)')" >&2
    exit 1
fi

# ---- 环境 -------------------------------------------------------------------
# 注意用重定向而不是管道：管道会让 export.sh 跑在子 shell 里，环境变量留不下来。
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    # shellcheck disable=SC1091
    . "$HOME/esp/esp-idf/export.sh" > /dev/null 2>&1 || true
fi
# **env 目录名里带 python 版本**（作者机器上是 idf6.1_py3.11_env，这台是
# idf6.1_py3.12_env）。写死会在别的机器上静默退回到系统 python3 —— 而系统
# python3 通常没有 esptool，报错会出现在很远的地方（导入包装器时）。
ESPTOOL_PY="${ESPTOOL_PY:-}"
if [ -z "$ESPTOOL_PY" ]; then
    for _py in "$HOME"/.espressif/python_env/idf*_env/bin/python; do
        [ -x "$_py" ] && { ESPTOOL_PY="$_py"; break; }
    done
fi
if [ ! -x "${ESPTOOL_PY:-}" ]; then
    ESPTOOL_PY="$(command -v python3 || true)"
fi
if [ -z "$ESPTOOL_PY" ] || [ ! -x "$ESPTOOL_PY" ]; then
    echo "错误: 找不到可用的 python（esptool 需要它）。" >&2
    exit 1
fi

# ---- 构建产物 ---------------------------------------------------------------
if [ ! -f "$BUILD_DIR/flash_args" ]; then
    echo "错误: 找不到 $BUILD_DIR/flash_args —— 先跑一次 'idf.py build'。" >&2
    exit 1
fi

echo "端口: $PORT"

# ---- 1. 切进下载模式 --------------------------------------------------------
# **这里以前做过一个"先用 esptool chip-id 检查是否已在 ROM 模式"的预检查，
# 那是个回归，已经删掉**：应用在跑的时候 esptool 连不上，而它失败退出时会
# 触碰 DTR/RTS —— 等于先把板子复位了一次，把后面的 knock 窗口搅乱。
#
# 结论：**分工要干净。**
#   flash.sh      假设应用在跑 -> 只负责 knock
#   flash-rom.sh  假设已在 ROM -> 什么都不敲，直接烧
# 想"自动判断"的代价比收益大得多（打开端口这个动作本身就是复位，见 RECOVERY.md）。
"$HERE/reset-to-download.sh" "$PORT"

# ---- 2. 烧写 -----------------------------------------------------------------
# flash_args 里的分区偏移和 bin 路径都是相对 build/ 的，必须进去再执行。
cd "$BUILD_DIR"
echo
echo "开始烧写..."
# 走包装器而不是直接 python -m esptool：ESP32-S31 的 ROM 有个
# FORCE_DOWNLOAD_BOOT 位，免按键进入下载模式时会把它置上，而 esptool 的
# hard-reset **不会清它** —— 结果烧完之后芯片又回到下载模式，应用永远不跑，
# 看起来像"烧成功了但板子不工作"。包装器在复位前先把这个位清掉。
"${ESPTOOL_PY}" "$HERE/tools/esp_mosaico_esptool_wrapper.py" esptool \
    --chip esp32s31 -p "$PORT" -b 460800 \
    --before no-reset --after hard-reset \
    write-flash @flash_args

echo
echo "✅ 烧写完成（全程没按 BOOT）"
