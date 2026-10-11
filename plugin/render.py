#!/usr/bin/env python3
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# render.py —— 把一页界面渲染成 480×480 的裸 RGB
#
# 约定
# ----
#   stdin  : 一页的 JSON（一行）
#   stdout : 480*480*3 字节裸 RGB（小端无关，就是 R,G,B 顺序）
#
# **为什么用裸 RGB 而不是 PNG**：调用方（render.js）拿去做 band 差分，本来就要
# 逐字节比对整幅图。PNG 得先解压才能比对，白白多一次编解码。
#
# 为什么渲染留在 Python
# --------------------
# 板子只有 512KB SRAM 跑不了 TTS/排版，界面必须在 PC 上画。而 DSH 自带的那份
# Python 里就有 Pillow（runtime/primary-runtime/dependencies/python），中文
# 字体直接用系统的 —— 这条链是现成的，没必要在 Node 里再搭一套。

import json
import sys
import os

from PIL import Image, ImageDraw, ImageFont

W = H = 480

BG = (0, 0, 0)          # 纯黑：AMOLED 上不发光，省电且对比度最高
SURFACE_HI = (26, 31, 41)
BORDER = (33, 40, 54)
TEXT = (240, 244, 250)
TEXT_DIM = (138, 147, 166)
TEXT_FAINT = (86, 94, 112)
ACCENT = (76, 194, 255)
OK = (61, 220, 151)
WARN = (255, 180, 84)
ERR = (255, 92, 122)
IDLE = (110, 120, 140)

STATE_COLORS = {
    "idle": IDLE, "thinking": ACCENT, "running": ACCENT, "editing": ACCENT,
    "testing": WARN, "waiting": WARN, "done": OK, "error": ERR, "failed": ERR,
    "tool": ACCENT,
}
STATE_LABELS = {
    "idle": "空闲", "thinking": "思考中", "running": "执行中", "editing": "改代码",
    "testing": "跑测试", "waiting": "等你决定", "done": "完成", "error": "出错",
    "failed": "失败", "tool": "调用工具",
}

_CJK = ["/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/Library/Fonts/Arial Unicode.ttf"]
_MONO = ["/System/Library/Fonts/SFNSMono.ttf",
         "/System/Library/Fonts/Supplemental/Andale Mono.ttf"]
_cache = {}


def font(size, mono=False):
    key = (size, mono)
    if key in _cache:
        return _cache[key]
    path = next((p for p in (_MONO if mono else _CJK) if os.path.exists(p)), None)
    f = ImageFont.truetype(path, size) if path else ImageFont.load_default()
    _cache[key] = f
    return f


def draw_status(d, img, job):
    state = job.get("state", "idle")
    color = STATE_COLORS.get(state, IDLE)
    label = job.get("label") or STATE_LABELS.get(state, state)

    # 状态点：外圈两层更暗的同色环，看起来"亮着"
    cx, cy, r = 34, 40, 7
    halo = Image.new("RGBA", (r * 8, r * 8), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    c = r * 4 - 1
    hd.ellipse((0, 0, r * 8 - 1, r * 8 - 1), fill=(*color, 28))
    hd.ellipse((c - r * 1.8, c - r * 1.8, c + r * 1.8, c + r * 1.8), fill=(*color, 70))
    img.paste(halo, (cx - r * 4, cy - r * 4), halo)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)

    d.text((58, 26), str(job.get("project", "agent"))[:22], font=font(22), fill=TEXT)
    secs = int(job.get("elapsed", 0))
    d.text((W - 24, 30), f"{secs // 60}:{secs % 60:02d}", font=font(20, True),
           fill=TEXT_DIM, anchor="ra")

    d.text((28, 90), label[:10], font=font(40), fill=color)

    y = 146
    if job.get("target"):
        d.text((28, y), str(job["target"])[:34], font=font(19, True), fill=TEXT_DIM)
        y += 28
    if job.get("detail"):
        d.text((28, y), str(job["detail"])[:30], font=font(19), fill=TEXT_DIM)

    progress = job.get("progress")
    if progress is not None:
        by = 214
        d.rounded_rectangle((28, by, W - 28, by + 12), radius=6, fill=SURFACE_HI)
        w = int((W - 56) * max(0.0, min(1.0, float(progress))))
        if w > 0:
            d.rounded_rectangle((28, by, 28 + max(w, 12), by + 12), radius=6, fill=color)
        d.text((W - 28, by + 24), f"{float(progress) * 100:.0f}%",
               font=font(18, True), fill=TEXT_DIM, anchor="ra")

    sy = 274
    d.line((28, sy, W - 28, sy), fill=BORDER, width=1)
    parts = []
    if job.get("files"):
        parts.append(f"改了 {job['files']} 个文件")
    if job.get("testsTotal"):
        parts.append(f"测试 {job.get('testsPassed', 0)}/{job['testsTotal']}")
    if parts:
        d.text((28, sy + 12), "   ·   ".join(parts), font=font(18), fill=TEXT_DIM)

    log = job.get("log") or []
    if log:
        # 行数跟按钮让位：按钮顶在 H-100，日志压过去就被盖住
        room = (H - 100) - (sy + 42)
        n = max(1, min(4, room // 26))
        ly = sy + 42
        for line in log[-n:]:
            d.text((28, ly), "·", font=font(17), fill=TEXT_FAINT)
            d.text((44, ly), str(line)[:36], font=font(17), fill=TEXT_FAINT)
            ly += 26


def draw_buttons(d, job):
    buttons = job.get("buttons") or []
    hits = []
    if not buttons:
        return hits
    state = job.get("state", "idle")
    color = STATE_COLORS.get(state, ACCENT)
    n = len(buttons)
    gap = 12
    bw = (W - 56 - gap * (n - 1)) // n
    by = H - 28 - 72
    for i, b in enumerate(buttons):
        bx = 28 + i * (bw + gap)
        fill = color if i == 0 else SURFACE_HI
        fg = (10, 12, 16) if i == 0 else TEXT
        d.rounded_rectangle((bx, by, bx + bw, by + 72), radius=14, fill=fill)
        d.text((bx + bw // 2, by + 36), str(b)[:8], font=font(24), fill=fg,
               anchor="mm")
        hits.append({"x": bx, "y": by, "w": bw, "h": 72, "value": str(b)})
    return hits


def render_status(job):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    draw_status(d, img, job)
    hits = draw_buttons(d, job)
    return img, hits


def render_ask(job):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, W, 4), fill=WARN)
    d.text((28, 40), "需要你决定", font=font(20), fill=WARN)

    q = str(job.get("question", ""))
    lines, cur = [], ""
    for ch in q:
        if len(cur) >= 14:
            lines.append(cur)
            cur = ""
        cur += ch
    if cur:
        lines.append(cur)
    qy = 84
    for ln in lines[:4]:
        d.text((28, qy), ln, font=font(30), fill=TEXT)
        qy += 40

    opts = (job.get("options") or [])[:5]
    n = max(1, len(opts))
    top = max(qy + 24, 240)
    bottom = H - (44 if job.get("footer") else 24)
    gap = 10
    bh = min(84, max(52, (bottom - top - gap * (n - 1)) // n))

    hits = []
    for i, opt in enumerate(opts):
        by = top + i * (bh + gap)
        d.rounded_rectangle((28, by, W - 28, by + bh), radius=14, fill=SURFACE_HI)
        d.text((52, by + bh // 2), str(i + 1), font=font(24, True), fill=ACCENT,
               anchor="lm")
        d.text((86, by + bh // 2), str(opt)[:16], font=font(26), fill=TEXT, anchor="lm")
        hits.append({"x": 28, "y": by, "w": W - 56, "h": bh, "value": str(i)})

    if job.get("footer"):
        d.text((28, H - 14), str(job["footer"])[:52], font=font(16), fill=TEXT_FAINT,
               anchor="ls")
    return img, hits


def render_boot(job):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((W // 2, H // 2 - 24), "mosaico-deck", font=font(34), fill=TEXT, anchor="mm")
    d.text((W // 2, H // 2 + 24), "等待 Harness…", font=font(20), fill=TEXT_DIM,
           anchor="mm")
    return img, []


PAGES = {"status": render_status, "ask": render_ask, "boot": render_boot}


def main():
    line = sys.stdin.readline()
    if not line:
        return 1
    job = json.loads(line)
    img, hits = PAGES.get(job.get("page", "status"), render_status)(job)

    if job.get("hits"):
        # 调用方要按钮位置：把命中区单独走 stderr 的旁路（stdout 只放像素，
        # 二进制流里混不进结构化的东西）。
        sys.stderr.write(json.dumps(hits) + "\n")
        sys.stderr.flush()

    # stdout 必须是干净的裸像素 —— 所以上面所有输出都走 stderr。
    sys.stdout.buffer.write(img.tobytes())
    sys.stdout.buffer.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
