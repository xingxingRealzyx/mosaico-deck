#!/usr/bin/env bash
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# check_port.sh —— 确认免按键烧录的关键逻辑还在
#
# 为什么需要它：这套逻辑**被误删过一次**（改 RX 回调时替换范围划大了，把
# LineStateChangedCallback 整段连带删掉）。而它的症状很有欺骗性 —— 不是编译不过，
# 而是"烧完读一次日志设备就没了"，很容易误判成硬件问题。
#
# 编译不会发现这种事，所以专门守一道。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$ROOT/components/usb_console/src/usb_console.cpp"

fail=0
need() {
    if grep -q "$1" "$SRC"; then
        printf "  ✅ %s\n" "$2"
    else
        printf "  ❌ 缺失: %s\n" "$2"
        fail=1
    fi
}

echo "=== 免按键烧录：关键逻辑自检 ==="
need "g_saw_esptool_arm"                        "武装标记（两步签名的第①步）"
need "第①步：武装"                             "武装分支"
need "第②步：交换"                             "交换分支（真正的触发点）"
need "RP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT\|LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT" \
                                                "FORCE_DOWNLOAD_BOOT 寄存器"
need "esp_restart"                              "重启"
need "callback_line_state_changed"              "控制线回调已注册"

echo
if [ "$fail" -ne 0 ]; then
    echo "❌ 免按键烧录的关键逻辑不完整，**不要烧录** —— 先对照 mosaico-fluidbox 恢复。"
    exit 1
fi
echo "✅ 免按键烧录逻辑完整"
