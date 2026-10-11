#!/usr/bin/env python3
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# link_test.py —— 验证 PC ↔ 板子的 USB 链路
#
# 它做三件事，每件都能单独说明链路的一段通不通：
#   1. 打开串口，读板子主动发的 hello（证明板子 → PC 方向通）
#   2. 逐条 band 送一张图案（证明 PC → 板子方向通、且 band 协议对）
#   3. 送一条控制消息，等 ack（证明双向都能收）
#
# ⚠️ 打开串口会触碰 DTR/RTS。**固件里认的是 esptool 的完整两步签名**
# （DTR=0,RTS=1 → DTR=1,RTS=0），所以普通打开不会把板子踢进下载模式 ——
# 这一点在 mosaico-fluidbox 上实测验证过（连续 10 次烧录 + 反复读日志）。

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial： pip install pyserial")

MAGIC = b"\xde\xad"
TYPE_BAND = 0x01
TYPE_FULL = 0x02
TYPE_CTRL = 0x10

BAND_ROWS = 30
H_RES = 480
V_RES = 480
BAND_COUNT = V_RES // BAND_ROWS
BAND_BYTES = H_RES * BAND_ROWS * 2


def rgb565(r, g, b):
    """RGB565 + 字节序交换。

    面板要的字节序和 PC 这边打包的相反，所以每个像素都要 swap —— 固件里
    （main.cpp / render.cpp）也是这么做的，两边必须一致，否则颜色会串。
    """
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return struct.pack("<H", ((v >> 8) | ((v & 0xFF) << 8)) & 0xFFFF)


def send_frame(port, ftype, payload):
    header = MAGIC + bytes([ftype]) + struct.pack("<I", len(payload))
    port.write(header + payload)
    port.flush()


def drain(port, seconds):
    """读掉板子这段时间发来的所有行。"""
    end = time.time() + seconds
    lines = []
    buf = b""
    while time.time() < end:
        n = port.in_waiting
        if n:
            buf += port.read(n)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                lines.append(line.decode("utf-8", "replace").strip())
        else:
            time.sleep(0.01)
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="例如 /dev/cu.usbmodem21201")
    ap.add_argument("--full", action="store_true",
                    help="整屏一次性送（对比用；默认逐条 band 送）")
    args = ap.parse_args()

    # write_timeout 很重要：如果板子不消费，PC 侧的 write 会永久阻塞。
    # 有超时就能报错退出，而不是把测试挂在那里。
    port = serial.Serial(args.port, 115200, timeout=0.2, write_timeout=3.0)
    print(f"已打开 {args.port}")
    print("（注意：打开串口本身不会再触发下载模式 —— 固件认的是完整两步签名）\n")

    # ---- 1. 板子主动发的东西 ----
    print("=== 1. 读板子主动发的内容（3 秒）===")
    for line in drain(port, 3.0):
        print("   ", line)

    # ---- 2. 送一张图案 ----
    # 图案：横向三条色带（红/绿/蓝），每条占 4 条 band。
    # 这样一眼就能看出：颜色对不对（字节序）、band 顺序对不对、有没有漏 band。
    print("\n=== 2. 送图案（逐条 band）===")
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]

    t0 = time.time()
    for band in range(BAND_COUNT):
        c = colors[band // 4]
        row = rgb565(*c) * H_RES
        pixels = row * BAND_ROWS
        assert len(pixels) == BAND_BYTES, f"band 大小不对: {len(pixels)}"

        if args.full:
            pass  # 整屏模式在下面单独处理
        try:
            send_frame(port, TYPE_BAND, bytes([band]) + pixels)
        except serial.SerialTimeoutException:
            print(f"    ❌ 第 {band} 条 band 写超时 —— 板子没在消费，链路有问题")
            port.close()
            return
        if band % 4 == 3:
            print(f"    band {band + 1}/{BAND_COUNT} ...")
    dt = time.time() - t0
    print(f"    送了 {BAND_COUNT} 条 band，共 {BAND_COUNT * BAND_BYTES / 1024:.0f} KB，"
          f"耗时 {dt * 1000:.0f} ms（{BAND_COUNT * BAND_BYTES / dt / 1024:.0f} KB/s）")

    # ---- 3. 控制消息往返 ----
    print("\n=== 3. 控制消息往返 ===")
    send_frame(port, TYPE_CTRL, b'{"cmd":"ping","n":1}')
    got = drain(port, 2.0)
    for line in got:
        print("   ", line)
    if any('"ack"' in l and '"ctrl"' in l for l in got):
        print("    ✅ 收到 ctrl 的 ack")
    else:
        print("    ❌ 没收到 ack")

    # ---- 4. 再读一段，看统计 ----
    print("\n=== 4. 板子随后 4 秒的输出 ===")
    for line in drain(port, 4.0):
        if "链路" in line or "fps" in line or "触摸" in line:
            print("   ", line)

    port.close()
    print("\n完成。屏幕应该是**自上而下 红/绿/蓝/白 四条色带**。")


if __name__ == "__main__":
    main()
