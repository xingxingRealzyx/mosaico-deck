# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# ui.py —— 把状态渲染成 480×480 的图
#
# 设计上的几个取舍
# --------------
# · **背景用纯黑 #000000。** 面板是 AMOLED，纯黑像素完全不发光 —— 既省电，
#   对比度也最高。灰底黑字在 AMOLED 上反而又费电又显脏。
# · **信息分三层**：状态色 + 大字号（一眼看到）→ 细节（等宽小字）→ 统计（次要色）。
#   小屏上最忌讳把所有信息同等对待。
# · **留白比塞满重要。** 480×480 在桌面上是"瞥一眼"的设备，不是阅读设备。
# · **按钮一定够大**：手指在 4.8cm 见方的屏上点，热区小于 60px 就开始误触。

from __future__ import annotations

from dataclasses import dataclass

from PIL import Image, ImageDraw, ImageFont

W = 480
H = 480
BAND_ROWS = 30
BAND_COUNT = H // BAND_ROWS

# ---------------------------------------------------------------------------
# 调色板
# ---------------------------------------------------------------------------
BG = (0, 0, 0)               # 纯黑：AMOLED 上不发光
SURFACE = (17, 20, 27)       # 卡片
SURFACE_HI = (26, 31, 41)    # 卡片高亮 / 按钮
BORDER = (33, 40, 54)
TEXT = (240, 244, 250)
TEXT_DIM = (138, 147, 166)
TEXT_FAINT = (86, 94, 112)

ACCENT = (76, 194, 255)      # 运行中：青
OK = (61, 220, 151)          # 成功：绿
WARN = (255, 180, 84)        # 等待：橙
ERR = (255, 92, 122)         # 失败：红
IDLE = (110, 120, 140)

STATE_COLORS = {
    "idle": IDLE,
    "thinking": ACCENT,
    "running": ACCENT,
    "editing": ACCENT,
    "testing": WARN,
    "waiting": WARN,
    "done": OK,
    "error": ERR,
    "failed": ERR,
}

STATE_LABELS = {
    "idle": "空闲",
    "thinking": "思考中",
    "running": "执行中",
    "editing": "改代码",
    "testing": "跑测试",
    "waiting": "等你决定",
    "done": "完成",
    "error": "出错",
    "failed": "失败",
}

# ---------------------------------------------------------------------------
# 字体
# ---------------------------------------------------------------------------
_CJK_CANDIDATES = [
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
]
_MONO_CANDIDATES = [
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
]

_font_cache: dict[tuple, ImageFont.FreeTypeFont] = {}


def _first_existing(paths: list[str]) -> str | None:
    import os

    for p in paths:
        if os.path.exists(p):
            return p
    return None


def font(size: int, mono: bool = False) -> ImageFont.FreeTypeFont:
    key = (size, mono)
    if key in _font_cache:
        return _font_cache[key]
    path = _first_existing(_MONO_CANDIDATES if mono else _CJK_CANDIDATES)
    f = ImageFont.truetype(path, size) if path else ImageFont.load_default()
    _font_cache[key] = f
    return f


# ---------------------------------------------------------------------------
# 组件
# ---------------------------------------------------------------------------
@dataclass
class Button:
    """屏幕上的一块可点区域。

    **布局是 PC 侧决定的，所以点击的含义也只有 PC 知道** —— 板子只上报
    "手指落在 (x,y)"，由这里做命中判定。这样板子保持成一个纯哑终端，
    改界面不用重新烧录。
    """

    x: int
    y: int
    w: int
    h: int
    value: str

    def hit(self, px: int, py: int) -> bool:
        # 热区上下左右各放宽 6px —— 手指比鼠标粗，边界上差几个像素很正常。
        pad = 6
        return (self.x - pad <= px <= self.x + self.w + pad) and (
            self.y - pad <= py <= self.y + self.h + pad
        )


def _text(d: ImageDraw.ImageDraw, xy, s, f, fill, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def _dot(img: Image.Image, cx: int, cy: int, r: int, color) -> None:
    """状态点：外面套两圈更暗的同色环，看起来像"亮着"。

    必须贴到原图上合成（不能直接在新图层上画完再 paste），因为纯黑背景下
    半透明环叠出来才有发光感。
    """
    halo = Image.new("RGBA", (r * 8, r * 8), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    c = r * 4 - 1
    hd.ellipse((0, 0, r * 8 - 1, r * 8 - 1), fill=(*color, 28))
    hd.ellipse((c - r * 1.8, c - r * 1.8, c + r * 1.8, c + r * 1.8), fill=(*color, 70))
    img.paste(halo, (cx - r * 4, cy - r * 4), halo)
    ImageDraw.Draw(img).ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)


def render_status(
    project: str,
    state: str,
    detail: str = "",
    target: str = "",
    progress: float | None = None,
    elapsed_s: float = 0.0,
    files_changed: int = 0,
    tests_passed: int = 0,
    tests_total: int = 0,
    log: list[str] | None = None,
    buttons: list[tuple[str, str]] | None = None,
) -> tuple[Image.Image, list[Button]]:
    """状态页。

    buttons 是 [(显示文字, 值)]。它们被安排在底部，高度 72px —— 手指点得准。
    """
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    color = STATE_COLORS.get(state, IDLE)
    label = STATE_LABELS.get(state, state)

    # ---- 顶部：状态点 + 项目名 + 计时 -------------------------------------
    _dot(img, 34, 40, 7, color)

    _text(d, (58, 26), project[:22], font(22), TEXT)
    mm, ss = divmod(int(elapsed_s), 60)
    _text(d, (W - 24, 30), f"{mm}:{ss:02d}", font(20, mono=True), TEXT_DIM, anchor="ra")

    # ---- 状态标题 ----------------------------------------------------------
    _text(d, (28, 90), label, font(40), color)

    y = 146
    if target:
        _text(d, (28, y), target[:34], font(19, mono=True), TEXT_DIM)
        y += 28
    if detail:
        _text(d, (28, y), detail[:30], font(19), TEXT_DIM)

    # ---- 进度条 ------------------------------------------------------------
    if progress is not None:
        by = 214
        d.rounded_rectangle((28, by, W - 28, by + 12), radius=6, fill=SURFACE_HI)
        w = int((W - 56) * max(0.0, min(1.0, progress)))
        if w > 0:
            d.rounded_rectangle((28, by, 28 + max(w, 12), by + 12), radius=6, fill=color)
        _text(d, (W - 28, by + 24), f"{progress * 100:.0f}%", font(18, mono=True),
              TEXT_DIM, anchor="ra")

    # ---- 统计行 ------------------------------------------------------------
    sy = 274
    d.line((28, sy, W - 28, sy), fill=BORDER, width=1)
    parts = []
    if files_changed:
        parts.append(f"改了 {files_changed} 个文件")
    if tests_total:
        parts.append(f"测试 {tests_passed}/{tests_total}")
    if parts:
        _text(d, (28, sy + 12), "   ·   ".join(parts), font(18), TEXT_DIM)

    # ---- 最近几步 ----------------------------------------------------------
    # **行数必须跟按钮让位**：按钮顶在 H-100，日志压过去就被盖住了
    #（第一版就是 4 行固定，最后一行被"批准/拒绝"挡掉）。
    if log:
        room = (H - 100) - (sy + 42)
        max_lines = max(1, min(4, room // 26))
        # 只留最近几条 —— 桌面上是"瞥一眼"，不是读日志
        ly = sy + 42
        for line in log[-max_lines:]:
            _text(d, (28, ly), "·", font(17), TEXT_FAINT)
            _text(d, (44, ly), line[:36], font(17), TEXT_FAINT)
            ly += 26

    # ---- 底部按钮 ----------------------------------------------------------
    hits: list[Button] = []
    if buttons:
        n = len(buttons)
        gap = 12
        bw = (W - 28 * 2 - gap * (n - 1)) // n
        by = H - 28 - 72
        for i, (text, value) in enumerate(buttons):
            bx = 28 + i * (bw + gap)
            # 第一个按钮用状态色（通常是"批准"），其余用中性面
            fill = color if i == 0 else SURFACE_HI
            fg = (10, 12, 16) if i == 0 else TEXT
            d.rounded_rectangle((bx, by, bx + bw, by + 72), radius=14, fill=fill)
            _text(d, (bx + bw // 2, by + 36), text[:8], font(24), fg, anchor="mm")
            hits.append(Button(bx, by, bw, 72, value))

    return img, hits


def render_ask(question: str, options: list[str], footer: str = "") -> tuple[Image.Image, list[Button]]:
    """问询页：一个大问题 + 若干大按钮。

    按钮高度按选项数自适应 —— 2 个选项时每个 84px，5 个时 62px，都还在手指
    能点准的范围里。超过 5 个就只显示前 5 个（再挤就点不准了）。
    """
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    # 顶部警示条
    d.rectangle((0, 0, W, 4), fill=WARN)

    _text(d, (28, 40), "需要你决定", font(20), WARN)

    # 问题正文：长了自动折行（按字符数硬折，中文够用）
    qy = 84
    f = font(30)
    line = ""
    lines = []
    for ch in question:
        if len(line) >= 14:
            lines.append(line)
            line = ""
        line += ch
    if line:
        lines.append(line)
    for ln in lines[:4]:
        _text(d, (28, qy), ln, f, TEXT)
        qy += 40

    # 选项按钮
    opts = options[:5]
    n = max(1, len(opts))
    top = max(qy + 24, 240)
    # 给底部的提示文字留出位置 —— 第一版算到 H-28，提示被按钮压掉了一半。
    bottom = H - (44 if footer else 24)
    avail = bottom - top
    gap = 10
    bh = min(84, max(52, (avail - gap * (n - 1)) // n))

    hits: list[Button] = []
    for i, opt in enumerate(opts):
        by = top + i * (bh + gap)
        d.rounded_rectangle((28, by, W - 28, by + bh), radius=14, fill=SURFACE_HI)
        # 序号用强调色，文字用主色
        _text(d, (52, by + bh // 2), f"{i + 1}", font(24, mono=True), ACCENT, anchor="lm")
        _text(d, (86, by + bh // 2), opt[:16], font(26), TEXT, anchor="lm")
        hits.append(Button(28, by, W - 56, bh, str(i)))

    if footer:
        _text(d, (28, H - 14), footer[:52], font(16), TEXT_FAINT, anchor="ls")

    return img, hits


def render_boot() -> Image.Image:
    """开机/未连接时的画面。"""
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    _text(d, (W // 2, H // 2 - 24), "mosaico-deck", font(34), TEXT, anchor="mm")
    _text(d, (W // 2, H // 2 + 24), "等待 bridge…", font(20), TEXT_DIM, anchor="mm")
    return img
