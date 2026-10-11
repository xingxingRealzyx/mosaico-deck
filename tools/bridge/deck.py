# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# deck.py —— 和板子通信的底层
#
# 只做一件事：把一张 480×480 的图送上去，把触摸/按键收回来。
# 界面长什么样、点了之后干什么，都不在这里 —— 那些在 ui.py / bridge.py。
#
# 最重要的一点：**band 差分**
# -------------------------
# 整屏 450KB 在 60MHz QSPI 上要 15.4ms，而面板刷新周期是 17ms —— 一帧的传输时间
# 几乎等于一次刷新，稍微一抖就会跨越刷新，画面撕裂（实测一根横扫的亮条会"弯"）。
#
# 而真实 UI 大部分时间是静态的：改一段文字也就影响一两条 band，1.4~2.8ms 传完，
# 稳稳落在一次刷新之内，**根本不会撕裂**。
#
# 所以这里逐条 band 比对，只发变化的那几条。实测：整屏 450KB / 300ms，
# 改一行字 ~2KB / 1.5ms —— 差 200 倍。

from __future__ import annotations

import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

import serial
from PIL import Image

MAGIC = b"\xde\xad"
TYPE_BAND = 0x01
TYPE_FULL = 0x02
TYPE_CTRL = 0x10
TYPE_AUDIO = 0x20   # PC -> 板子：放音 PCM
TYPE_REC = 0x21     # PC -> 板子：录音控制

H_RES = 480
V_RES = 480
BAND_ROWS = 30
BAND_COUNT = V_RES // BAND_ROWS
BAND_PIXELS = H_RES * BAND_ROWS


def _rgb565_bytes(img: Image.Image, y0: int, y1: int) -> bytes:
    """把 img 的第 y0..y1 行转成面板要的 RGB565 字节流。

    面板和 CPU 的字节序相反，所以每个像素都要 swap —— 固件侧（main.cpp /
    render.cpp）也是这么做的，两边必须一致，否则颜色会串。
    """
    crop = img.crop((0, y0, H_RES, y1)).convert("RGB")
    px = crop.tobytes()  # RGBRGBRGB...
    out = bytearray(len(px) // 3 * 2)
    j = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        v = ((v >> 8) | ((v & 0xFF) << 8)) & 0xFFFF
        out[j] = v & 0xFF
        out[j + 1] = (v >> 8) & 0xFF
        j += 2
    return bytes(out)


@dataclass
class Stats:
    bands_sent: int = 0
    bytes_sent: int = 0
    full_sends: int = 0
    last_send_ms: float = 0.0


class Deck:
    """一块 mosaico-deck 板子。

    线程安全：发送有锁。事件由后台线程读，通过回调交出去。
    """

    def __init__(self, port: str, on_event: Callable[[dict], None] | None = None,
                 on_audio: Callable[[bytes], None] | None = None):
        self.port = port
        self.on_event = on_event
        self.on_audio = on_audio
        self._ser: serial.Serial | None = None
        self._tx = threading.Lock()
        self._last: Image.Image | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self.stats = Stats()
        self._rx_buf = b""
        self._pending_audio: int | None = None

    # ---- 连接 --------------------------------------------------------------
    def open(self, wait_ready: float = 6.0) -> None:
        # write_timeout 很重要：如果板子不消费，PC 侧的 write 会永久阻塞。
        # 有超时就报错退出，而不是把程序挂在那里。这一点是踩出来的 ——
        # 曾经因为板子侧环形缓冲满而 break 掉 USB 读取，两边一起卡死。
        self._ser = serial.Serial(self.port, 115200, timeout=0.05, write_timeout=5.0)

        # 等板子把启动日志打完，顺便把 hello 之类的吞掉。
        deadline = time.time() + wait_ready
        while time.time() < deadline:
            self._pump_rx()
            time.sleep(0.02)

        self._thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        if self._ser is not None:
            self._ser.close()
            self._ser = None

    # ---- 发送 --------------------------------------------------------------
    def _send_frame(self, ftype: int, payload: bytes) -> None:
        assert self._ser is not None
        with self._tx:
            self._ser.write(MAGIC + bytes([ftype]) + struct.pack("<I", len(payload)) + payload)
            self._ser.flush()

    def send_image(self, img: Image.Image, force_full: bool = False) -> int:
        """送一张图。**逐条 band 比对，只发变化的那几条。**

        返回发出的 band 数 —— 这个数直接反映"这次更新有多轻"。
        """
        assert img.size == (H_RES, V_RES), f"图必须是 480×480，收到 {img.size}"

        changed: list[int] = []
        for b in range(BAND_COUNT):
            if force_full or self._last is None:
                changed.append(b)
                continue
            y0 = b * BAND_ROWS
            strip = img.crop((0, y0, H_RES, y0 + BAND_ROWS))
            prev = self._last.crop((0, y0, H_RES, y0 + BAND_ROWS))
            if strip.tobytes() != prev.tobytes():
                changed.append(b)

        if not changed:
            return 0

        t0 = time.time()
        for b in changed:
            y0 = b * BAND_ROWS
            self._send_frame(TYPE_BAND, bytes([b]) + _rgb565_bytes(img, y0, y0 + BAND_ROWS))

        self._last = img.copy()
        self.stats.bands_sent += len(changed)
        self.stats.bytes_sent += len(changed) * (BAND_PIXELS * 2)
        if len(changed) == BAND_COUNT:
            self.stats.full_sends += 1
        self.stats.last_send_ms = (time.time() - t0) * 1000
        return len(changed)

    # ---- 音频 --------------------------------------------------------------
    def speak_pcm(self, pcm: bytes, chunk: int = 2048, pace: bool = True) -> float:
        """把 48kHz 交错立体声 16 位 PCM 推给板子播放。返回耗时秒数。

        **必须按播放速率发送，不能一口气灌进去。**

        这里踩过一个坑：一开始分块 8192 字节、循环里直接连着发，结果 99 个分块
        板子只收到 5 个 —— 板子的 RX 环形缓冲（32KB）瞬间被灌满，剩下的全丢了。

        原因是板子**边收边播**，而播放是实时的：`audio::Play()` 会阻塞到 I2S DMA
        收下这段，所以板子的消费速度被钉死在 192KB/s（48kHz×立体声×16位）。
        PC 一口气发是 ~1.4MB/s，比它快 7 倍以上，缓冲再大也没用。

        所以按真实速率发。代价是"发送"本身要花掉音频时长，但反正它也要播这么久
        —— 而且**第一个分块立刻就播**，所以从调用到出声只有十几毫秒。

        **超前量必须控制住，不能拍脑袋定。** 第一次用 0.9 倍速（超前 11%），
        4.2 秒的语音攒出 88KB 的超前，而板子的环形缓冲只有 32KB —— 照样溢出。
        0.97 的话 4.2 秒只超前 16KB，稳在缓冲之内。
        """
        bytes_per_sec = 48000 * 4  # 48kHz × 立体声 × 16bit
        t0 = time.time()
        sent = 0
        for i in range(0, len(pcm), chunk):
            piece = pcm[i:i + chunk]
            self._send_frame(TYPE_AUDIO, piece)
            sent += len(piece)
            if pace and len(piece) == chunk:
                # 目标：发完 N 字节时，理想时刻是 N/bytes_per_sec。
                # 0.97 = 超前 3%：4 秒的音频只领先约 16KB，而板子的环形缓冲是
                # 64KB —— 既保证不欠载，又不会把它灌满。
                target = t0 + sent / bytes_per_sec * 0.97
                slack = target - time.time()
                if slack > 0:
                    time.sleep(slack)
        return time.time() - t0

    def start_capture(self) -> None:
        self._send_frame(TYPE_REC, b'{"cmd":"start"}')

    def stop_capture(self) -> None:
        self._send_frame(TYPE_REC, b'{"cmd":"stop"}')

    def send_ctrl(self, obj: dict) -> None:
        import json

        self._send_frame(TYPE_CTRL, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def invalidate(self) -> None:
        """丢掉缓存的"上一帧"，下一次发送就是整屏。"""
        self._last = None

    # ---- 接收 --------------------------------------------------------------
    def _pump_rx(self) -> None:
        if self._ser is None:
            return
        n = self._ser.in_waiting
        if n:
            self._rx_buf += self._ser.read(n)

        while True:
            # 先看有没有"音频头 + 裸数据"待收。
            #
            # 板子到 PC 这个方向平时全是 JSON 行，只有录音会发二进制：先一行
            # {"t":"audio","bytes":N}，紧跟 N 字节 PCM。所以要先按行找到那个头，
            # 再按长度把裸数据读走 —— **不能当行读**，PCM 里全是 \n。
            if self._pending_audio is not None:
                need = self._pending_audio
                if len(self._rx_buf) < need:
                    return  # 等数据
                pcm = self._rx_buf[:need]
                self._rx_buf = self._rx_buf[need:]
                self._pending_audio = None
                if self.on_audio is not None:
                    self.on_audio(pcm)
                continue

            nl = self._rx_buf.find(b"\n")
            if nl < 0:
                return
            line, self._rx_buf = self._rx_buf[:nl], self._rx_buf[nl + 1:]
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            # 板子把日志和协议混在同一个 CDC 上，协议行一律以 { 开头。
            if not text.startswith("{"):
                continue
            import json

            try:
                ev = json.loads(text)
            except json.JSONDecodeError:
                continue
            if ev.get("t") == "audio" and isinstance(ev.get("bytes"), int):
                # 记下要读多少裸数据，下一轮循环处理
                self._pending_audio = ev["bytes"]
                continue
            if self.on_event is not None:
                self.on_event(ev)

    def _rx_loop(self) -> None:
        while not self._stop.is_set():
            try:
                self._pump_rx()
            except (serial.SerialException, OSError):
                break
            time.sleep(0.005)


def find_port() -> str | None:
    """找 mosaico-deck 的串口。"""
    import glob

    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None
