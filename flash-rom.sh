#!/usr/bin/env bash
# =============================================================================
#  flash-rom.sh —— 板子**已经在 ROM 下载模式**时的烧录脚本
#
#  和 flash.sh 的区别
#  ------------------
#  flash.sh        假设应用固件在跑，先用 DTR/RTS 把它"敲"进下载模式再烧。
#  flash-rom.sh    假设芯片**已经在** ROM 下载模式，直接烧，不敲任何东西。
#
#  为什么需要这个
#  --------------
#  **ROM 下载模式里 DTR/RTS 就是硬复位信号。** 如果芯片已经在 ROM 里，再去敲
#  一下，它就会退出下载模式、跑起应用 —— 正好把你要烧的窗口弄没了。
#
#  另一个同样致命的坑：**打开 /dev/cu.usbmodem* 这个动作本身就会拉 DTR/RTS。**
#  所以任何"先打开端口看看在不在"的探测代码都会把 ROM 踢出去。这个脚本因此
#  只用 `[ -e "$PORT" ]` 判断存在性，绝不预先 open，端口只交给 esptool 打开一次。
#
#  用法
#  ----
#    ./flash-rom.sh                        # 自动找端口
#    ./flash-rom.sh /dev/cu.usbmodem21201
#    ./flash-rom.sh --wait 60              # 最多等 60 秒端口出现
#
#  进 ROM 下载模式的办法（关机状态下）：
#    长按 POWER 关机 -> 按住 BOOT 不放 -> 短按 POWER 开机 -> 继续按住 BOOT 十秒
#
# =============================================================================
set -euo pipefail

WAIT_SECS=30
PORT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --wait) WAIT_SECS="$2"; shift 2 ;;
        -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) PORT="$1"; shift ;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"

if [ ! -f "$BUILD/flash_args" ]; then
    echo "错误: 找不到 $BUILD/flash_args，请先 idf.py build。" >&2
    exit 1
fi

# 算上 bootloader 和分区表，完整的烧录清单。
IMAGES=(
    0x2000  "$BUILD/bootloader/bootloader.bin"
    0x8000  "$BUILD/partition_table/partition-table.bin"
    0x10000 "$BUILD/mosaico-deck.bin"
)
for ((i = 1; i < ${#IMAGES[@]}; i += 2)); do
    if [ ! -f "${IMAGES[$i]}" ]; then
        echo "错误: 缺少 ${IMAGES[$i]}，请先 idf.py build。" >&2
        exit 1
    fi
done

# 找端口：**只用 -e 判断，绝不 open**。
find_port() {
    if [ -n "$PORT" ] && [ -e "$PORT" ]; then
        echo "$PORT"; return 0
    fi
    local p
    for p in /dev/cu.usbmodem*; do
        [ -e "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

echo "等待端口出现（最多 ${WAIT_SECS} 秒，期间绝不打开端口）..."
FOUND=""
for _ in $(seq 1 "$WAIT_SECS"); do
    if FOUND="$(find_port)"; then break; fi
    sleep 1
done

if [ -z "$FOUND" ]; then
    cat >&2 <<'EOF'
错误: 没有找到 /dev/cu.usbmodem*。

板子没在 USB 上。请按顺序试：
  1. 长按 POWER 关机（确认橙色灯灭），再按住 BOOT 开机（按住十秒）
  2. 换一根**能传数据**的 Type-C 线，换 Mac 上另一个 C 口
  3. 板子有 65mAh 电池，拔 USB 不等于断电；必要时断开电池做真正的冷启动
  4. 兜底通道：右侧模块口的 TX/RX + USB-TTL 模块走 UART 烧录，完全绕开 USB

详见同目录 RECOVERY.md。
EOF
    exit 1
fi

echo "端口: $FOUND"
echo "开始烧录（一次烧完，中途绝不打断）..."

# env 目录名里带 python 版本，不要写死（见 flash.sh 里的同一条注释）。
if [ -z "${ESPTOOL_PY:-}" ]; then
    for _py in "$HOME"/.espressif/python_env/idf*_env/bin/python; do
        [ -x "$_py" ] && { ESPTOOL_PY="$_py"; break; }
    done
fi
ESPTOOL_PY="${ESPTOOL_PY:-}"
PY="${PY:-}"
if [ -z "$PY" ]; then
    if [ -x "$ESPTOOL_PY" ]; then PY="$ESPTOOL_PY"; else PY="$(command -v python3)"; fi
fi

# --before no-reset：不要再复位，芯片已经在下载模式了。
# --after hard-reset：烧完让它跑新固件。
cd "$BUILD"
exec "${PY}" "$HERE/tools/esp_mosaico_esptool_wrapper.py" esptool \
    --chip esp32s31 \
    -p "$FOUND" \
    -b "${BAUD:-115200}" \
    --before no-reset \
    --after hard-reset \
    write-flash @flash_args
