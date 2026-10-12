#!/usr/bin/env bash
# =============================================================================
#  check-board.sh —— 从 Mac 这边判断板子现在处于什么状态
#
#  为什么需要它
#  ----------
#  **板子上看不出来。** ROM 下载模式和应用态长得一模一样：屏幕都是黑的、
#  灯都不亮（本工程没有驱动 GPIO3 那颗橙色状态灯）。唯一可靠的判据在主机侧。
#
#  三个层次的判据，从弱到强：
#    1. /dev/cu.usbmodem* 存在吗        -> 主机建好串口节点了吗
#    2. ioreg 里能看到 ESP32-S31 吗     -> 芯片在 USB 上枚举了吗
#    3. esptool chip-id 能连上吗        -> 芯片在不在 ROM 下载模式
#
#  ⚠️ 第 3 步会打开串口。**打开 /dev/cu.usbmodem* 这个动作本身就会拉 DTR/RTS**，
#     而 ROM 下载模式里 DTR/RTS 就是硬复位信号。不过 esptool 加了
#     --before no-reset，实测连接成功、不会把芯片踢出去。
#     所以第 3 步默认执行，但用 -s / --safe 可以跳过它。
#
#  用法:
#      ./check-board.sh            # 完整检查（含 esptool chip-id）
#      ./check-board.sh --safe     # 只做不碰串口的两步
#      ./check-board.sh --dump     # 顺便读 3 秒串口日志
# =============================================================================
set -uo pipefail

SAFE=0
DUMP=0
for a in "$@"; do
    case "$a" in
        -s|--safe) SAFE=1 ;;
        -d|--dump) DUMP=1 ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    esac
done

# env 目录名里带 python 版本，不要写死（见 flash.sh 里的同一条注释）。
if [ -z "${ESPTOOL_PY:-}" ]; then
    for _py in "$HOME"/.espressif/python_env/idf*_env/bin/python; do
        [ -x "$_py" ] && { ESPTOOL_PY="$_py"; break; }
    done
fi
PY="${ESPTOOL_PY:-$(command -v python3)}"
[ -x "$PY" ] || PY="$(command -v python3)"

echo "=================== 板子状态检查 ==================="
echo

# ---- 1. 串口节点 -------------------------------------------------------------
# 注意：只判断存在性，绝不 open。
PORT=""
for p in /dev/cu.usbmodem*; do
    [ -e "$p" ] && { PORT="$p"; break; }
done
if [ -n "$PORT" ]; then
    echo "[1/3] 串口节点: ✅ $PORT"
else
    echo "[1/3] 串口节点: ❌ 没有 /dev/cu.usbmodem*"
fi

# ---- 2. USB 描述符 -----------------------------------------------------------
NAME=""
if command -v ioreg >/dev/null 2>&1; then
    while IFS= read -r line; do
        case "$line" in
            *'"USB Product Name"'*ESP32*)
                NAME="$(printf '%s' "$line" | sed -E 's/.*= *"(.*)".*/\1/')"
                break ;;
        esac
    done < <(ioreg -p IOUSB -w0 -l 2>/dev/null)
fi
if [ -n "$NAME" ]; then
    echo "[2/3] USB 枚举:  ✅ 看到 \"$NAME\""
else
    echo "[2/3] USB 枚举:  ❌ ioreg 里没有任何 ESP32 设备"
fi

# ---- 3. 是不是在 ROM 下载模式 ------------------------------------------------
if [ "$SAFE" -eq 1 ]; then
    echo "[3/3] 下载模式:  (--safe，跳过)"
elif [ -z "$PORT" ]; then
    echo "[3/3] 下载模式:  ❌ 没有端口，无法判断"
else
    echo "[3/3] 下载模式:  正在用 chip-id 探测（--before no-reset）..."
    if OUT="$("$PY" -m esptool --chip esp32s31 -p "$PORT" \
              --before no-reset --after no-reset chip-id 2>&1)"; then
        MAC="$(printf '%s' "$OUT" | sed -nE 's/.*MAC: *([0-9a-f:]+).*/\1/p' | head -1)"
        # **The braces are required.** macOS ships bash 3.2, which folds a following
        # multibyte character into the variable name: `$MAC）` is read as the variable
        # `MAC）`, and under `set -u` that is a hard "unbound variable" error. So this
        # line used to crash on the *success* path -- exactly the branch that reports the
        # one answer this script exists to give. `${MAC}` delimits the name.
        if [ -n "$MAC" ]; then
            echo "                 ✅ 在 ROM 下载模式（MAC ${MAC}）"
        else
            echo "                 ✅ 在 ROM 下载模式"
        fi
        echo "                 -> 直接跑 ./flash-rom.sh"
    else
        echo "                 ❌ 连不上 —— 芯片在跑应用固件，或者 USB 链路有问题"
        echo "                 -> 先 ./flash.sh（它会用 DTR/RTS 把芯片敲进下载模式）"
    fi
fi

# ---- 可选：读串口日志 --------------------------------------------------------
if [ "$DUMP" -eq 1 ] && [ -n "$PORT" ]; then
    echo
    echo "---- 读 3 秒串口日志 ----"
    "$PY" - "$PORT" <<'PY' 2>/dev/null || true
import sys, time, serial
try:
    s = serial.Serial(sys.argv[1], 115200, timeout=0.3)
except Exception as e:
    print("  打不开:", e); raise SystemExit
t0 = time.time(); buf = b""
while time.time() - t0 < 3.0:
    try:
        d = s.read(4096)
        if d: buf += d
    except Exception:
        print("  (端口断开)"); break
try: s.close()
except Exception: pass
sys.stdout.write(buf.decode("utf-8", "replace") if buf else "  (没有输出)\n")
PY
fi

echo
echo "==================================================="
if [ -z "$NAME" ]; then
    cat <<'EOF'

芯片完全没有出现在 USB 上。按顺序试：
  1. 长按 POWER 关机（确认橙色灯灭）-> 按住 BOOT -> 短按 POWER 开机 -> 按住十秒
  2. 换一根**能传数据**的 Type-C 线，换 Mac 上另一个 C 口
  3. 板子有 65mAh 电池，拔 USB 不等于断电；必要时断开电池做真正的冷启动
  4. 兜底：右侧模块口 TX/RX + USB-TTL 模块走 UART 烧录，完全绕开 USB

详见 RECOVERY.md。
EOF
fi
