#!/usr/bin/env bash
# =============================================================================
#  reset-to-download.sh —— 把 ESP-Mosaico 切进 ROM 下载模式（不用按 BOOT）
#
#  原理
#  ----
#  esptool 复位芯片时会在 CDC 控制线上依次打出 DTR/RTS 时序：
#
#       DTR=0, RTS=1  ->  DTR=1, RTS=0  ->  DTR=0, RTS=0
#                          ^^^^^^^^^^^^
#                          关键：RTS 下降沿时 DTR 为高
#
#  本工程的 usb_console 组件在 CDC_EVENT_LINE_STATE_CHANGED 回调里认这个沿，
#  置位 LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT 后重启，ROM 起来就直接进下载模式。
#  这个脚本只负责"发出请求 + 等 ROM 的 USB 重新枚举出来"，不碰 esptool。
#
#  为什么必须分成两步
#  ----------------
#  S31 rev0 的 ROM chip_usb_dw_prepare_persist 是空 stub，USB 不能跨复位存活。
#  所以 esptool 的 --before default-reset（"复位后还在同一个已打开的串口 fd 上
#  重连"）必然失败，报 `Could not configure port: (6, 'Device not configured')`。
#  见 esp-idf issue #18992。只能先在这里把芯片切进下载模式并等端口回来，再用
#  --before no-reset 去烧。
#
#  用法: ./reset-to-download.sh [PORT]
#         PORT=/dev/cu.usbmodem21201 ./reset-to-download.sh
# =============================================================================
set -euo pipefail

PORT="${1:-${PORT:-/dev/cu.usbmodem21201}}"

# 需要 pyserial。优先用 IDF 自带的 python 环境（esptool/pyserial 都在里面），
# 找不到再退回 PATH 上的 python3。
ESPTOOL_PY="${ESPTOOL_PY:-$HOME/.espressif/python_env/idf6.1_py3.11_env/bin/python}"
PY="${PY:-$ESPTOOL_PY}"
if [ ! -x "$PY" ]; then
    PY="$(command -v python3 || true)"
fi
if [ -z "$PY" ] || [ ! -x "$PY" ]; then
    echo "错误: 找不到可用的 python3（需要 pyserial）。" >&2
    exit 1
fi

WAIT_SECS="${WAIT_SECS:-15}"

echo "端口: $PORT"
echo "1/2 发出下载模式请求..."

# DTR/RTS 时序和 esptool 的 ClassicReset 一致。写完立刻 close，否则芯片重启
# 后这个 fd 已经作废（USB 重新枚举），留着只会让后续操作拿到 ENXIO。
"$PY" - "$PORT" <<'PY'
import sys
import time

import serial

port = sys.argv[1]
try:
    s = serial.Serial(port, 115200, timeout=1)
except serial.SerialException as exc:
    # 打不出时序时给一句人话，别让 esptool 之后报一堆 ENXIO 让人猜。
    print("    错误: 无法打开 %s: %s" % (port, exc), file=sys.stderr)
    print("    提示: 板子插好了吗？端口被别的程序占用了吗？", file=sys.stderr)
    sys.exit(1)

s.setDTR(False)   # IO0 = HIGH
s.setRTS(True)    # EN  = LOW  (复位)
time.sleep(0.10)
s.setDTR(True)    # IO0 = LOW
s.setRTS(False)   # EN  = HIGH (放开复位) <- 固件在这个下降沿识别
time.sleep(0.05)
s.setDTR(False)   # IO0 = HIGH
s.close()
print("    已发出下载模式请求:", port)
PY

# 固件要等约 100 ms 才重启，然后 USB 断开重连。先等节点消失（有些情况下
# ROM 枚举太快、节点根本来不及消失，所以这里的超时只是"别等太久"，不算失败）。
echo "2/2 等待 ROM 的 USB 重新枚举..."
for _ in $(seq 1 "$WAIT_SECS"); do
    [ -e "$PORT" ] || break
    sleep 0.2
done

# 等节点重新出现。
for _ in $(seq 1 $((WAIT_SECS * 5))); do
    if [ -e "$PORT" ]; then
        # 节点出现不等于枚举完成，稍微等一下再交给 esptool。
        sleep 0.5
        echo "ROM 下载模式就绪: $PORT"
        exit 0
    fi
    sleep 0.2
done

echo "警告: 端口 $PORT 未在 ${WAIT_SECS} 秒内出现" >&2
echo "      当前可用: $(ls /dev/cu.usbmodem* 2>/dev/null || echo '(无)')" >&2
exit 1
