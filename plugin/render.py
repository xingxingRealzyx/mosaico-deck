#!/usr/bin/env python3
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# render.py -- render one page of UI into raw 480x480 RGB
#
# Contract
# --------
#   stdin  : one page of JSON (single line)
#   stdout : 480*480*3 bytes of raw RGB (endianness-agnostic, just R,G,B order)
#   stderr : one line of JSON with the tap rectangles, when job["hits"] is set
#
# Pages
# -----
#   status  the default: state, tool, detail, log tail
#   image   one picture from disk, fitted, with a caption strip
#   ask     a question with tap targets
#   boot    the idle splash
#
# **Why raw RGB instead of PNG**: the caller (render.js) uses it for band
# diffing and already compares the whole image byte by byte. PNG would have to
# be decompressed first, wasting an extra encode/decode round.
#
# Why rendering stays in Python
# -----------------------------
# The board only has 512KB SRAM and cannot run TTS/layout, so the UI must be
# drawn on the PC. The Python shipped with DSH already includes Pillow
# (runtime/primary-runtime/dependencies/python), and CJK fonts come straight
# from the system -- this chain is ready-made, no need to build another in Node.
#
# Lessons this file learned the hard way (all seen on the real panel)
# -------------------------------------------------------------------
# 1. **Lay out from the space you have, never from a minimum.** The ask page used
#    to floor the option height at 52px: with five options and a three-line
#    question the last button was drawn *below the panel* while its tap rectangle
#    stayed live, and the fourth overlapped the footer. Every vertical budget here
#    is subtracted from what is actually left, and text is truncated to fit rather
#    than allowed to run over.
# 2. **Measure text in pixels.** Characters are not a unit of width -- a CJK glyph
#    is ~2x an ASCII one -- so wrapping by `len()` both wasted space and cut lines
#    off past the edge.
# 3. **No monospace face and no thin one.** Paths used to be drawn with SFNSMono,
#    which has no CJK glyphs (a Chinese directory simply vanished) and is thin
#    enough that grey on black stops being readable. One bold face for everything.
# 4. **Dimming is not hierarchy.** White -> grey -> darker grey meant the third
#    step was unreadable on black. There are exactly two text colours now and the
#    second one is bright; hierarchy comes from size and weight.

import json
import sys
import os

from PIL import Image, ImageDraw, ImageFont, ImageOps

W = H = 480

BG = (0, 0, 0)               # Pure black: emits nothing on AMOLED, max contrast
BORDER = (84, 95, 116)
TEXT = (252, 253, 255)
TEXT_SOFT = (214, 223, 238)
ACCENT = (110, 200, 255)
OK = (90, 236, 172)
WARN = (255, 200, 110)
ERR = (255, 120, 145)
IDLE = (176, 186, 206)

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

# On icons, and on colouring by tool
# ----------------------------------
# Both were tried on the panel and both were rejected: colour emoji read as a toy,
# and giving each family of tool its own hue turned a quiet status display into a
# rainbow. The colour here carries **state** -- idle / working / waiting / failed --
# which is the one thing worth reading from across the room, and nothing else.
# Which tool is running is already in the detail line, in words.

S_PROJECT = 26
S_ELAPSED = 24
S_LABEL = 46
S_TARGET = 24
S_DETAIL = 28
S_PROGRESS = 21
S_LOG = 22
S_ASK_LABEL = 24
S_ASK_Q = 30
S_ASK_OPT = 30
S_ASK_OPT_SMALL = 25
S_FOOTER = 19
S_BOOT = 36

# Picture preview box, inside the status page. Tuned by eye on the panel: big enough
# to recognise a screenshot's layout, small enough that the status stays readable.
IMG_MAX_W = W - 56
IMG_MAX_H = 250

# Every entry is (path, face index inside the file).
#
# **Index 2, not 1.** This .ttc holds four faces and the numbering is a trap:
#
#     index 0  Hiragino Sans GB / W3                (the thin default this used before)
#     index 1  .Hiragino Sans GB Interface / W3     (thinner still -- a UI variant)
#     index 2  Hiragino Sans GB / W6                <- 41% more ink than W3
#     index 3  .Hiragino Sans GB Interface / W6
#
# Index 1 looks bold-ish next to index 0 in a screenshot and is in fact *lighter*;
# only measuring the drawn ink settles it. plugin/test_render_layout.py asserts the
# chosen face stays heavier than W3 so this cannot silently regress.
_FACES = [("/System/Library/Fonts/Hiragino Sans GB.ttc", 2),
          ("/System/Library/Fonts/STHeiti Medium.ttc", 0),
          ("/Library/Fonts/Arial Unicode.ttf", 0)]
_font_cache = {}


def font(size, mono=False):
    """The one face, at `size`.

    `mono` is accepted and ignored so call sites read the same as before.
    """
    if size in _font_cache:
        return _font_cache[size]
    f = None
    for path, index in _FACES:
        if not os.path.exists(path):
            continue
        try:
            f = ImageFont.truetype(path, size, index=index)
            break
        except Exception:
            continue
    if f is None:
        f = ImageFont.load_default()
    _font_cache[size] = f
    return f


def font_for(text, size, mono=False):
    """Kept for call-site clarity; there is only one face now."""
    return font(size)


def width_of(text, f):
    """Pixel width of one string in one font."""
    return ImageDraw.Draw(Image.new("RGB", (1, 1))).textlength(str(text), font=f)


def fit(text, f, max_w):
    """Truncate to `max_w` pixels, with an ellipsis when something was dropped."""
    text = str(text)
    if width_of(text, f) <= max_w:
        return text
    out = ""
    for ch in text:
        if width_of(out + ch + "…", f) > max_w:
            break
        out += ch
    return (out + "…") if out else ""


def wrap(text, f, max_w, max_lines=None):
    """Wrap on pixel width, breaking between characters (CJK has no spaces)."""
    lines, cur = [], ""
    for ch in str(text):
        if ch == "\n":
            lines.append(cur)
            cur = ""
            continue
        if cur and width_of(cur + ch, f) > max_w:
            lines.append(cur)
            cur = ch
        else:
            cur += ch
    if cur:
        lines.append(cur)
    if max_lines is not None and len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] = fit(lines[-1] + "…", f, max_w)
    return lines


def parse_log(entry):
    """(kind, text, tool) from either a structured entry or a bare string."""
    if isinstance(entry, dict):
        return (entry.get("kind", "info"), str(entry.get("text", "")),
                entry.get("tool"))
    return "info", str(entry), None


def draw_image_preview(img, d, path, y):
    """Fit the picture at `path` into the preview box whose top edge is `y`.

    Returns (drawn height, "WxH" of the source, error string).

    Deliberately **not** a full-screen takeover: the status header and the state label
    stay visible above it. An earlier version replaced the whole panel with the
    picture, which hid exactly the information the panel exists to show.
    """
    box_w, box_h = IMG_MAX_W, IMG_MAX_H
    try:
        with Image.open(path) as src:
            # draft() lets libjpeg decode straight to a smaller size, so a 50MP photo
            # does not cost seconds and hundreds of MB on the way to 480px.
            src.draft("RGB", (box_w * 2, box_h * 2))
            pic = ImageOps.exif_transpose(src).convert("RGB")
        dims = f"{pic.width}×{pic.height}"
        pic.thumbnail((box_w, box_h), Image.LANCZOS)
        x = (W - pic.width) // 2
        img.paste(pic, (x, y))
        # A thin outline: a dark screenshot on a black background would otherwise have
        # no visible edge at all.
        d.rectangle((x - 1, y - 1, x + pic.width, y + pic.height),
                    outline=BORDER, width=1)
        return pic.height, dims, ""
    except Exception as exc:
        return 0, "", str(exc)


def draw_status(d, img, job):
    state = job.get("state", "idle")
    label = job.get("label") or STATE_LABELS.get(state, state)
    # The job also carries `tool`, deliberately unused for styling: which tool is
    # running belongs in words on the detail line, not in a second colour scale.
    color = STATE_COLORS.get(state, IDLE)

    # Header: status dot + project, elapsed time on the right.
    cx, cy, r = 34, 38, 9
    halo = Image.new("RGBA", (r * 8, r * 8), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    c = r * 4 - 1
    hd.ellipse((0, 0, r * 8 - 1, r * 8 - 1), fill=(*color, 30))
    hd.ellipse((c - r * 1.8, c - r * 1.8, c + r * 1.8, c + r * 1.8), fill=(*color, 76))
    img.paste(halo, (cx - r * 4, cy - r * 4), halo)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)

    secs = int(job.get("elapsed", 0))
    stamp = f"{secs // 60}:{secs % 60:02d}"
    f_stamp = font(S_ELAPSED, True)
    stamp_w = width_of(stamp, f_stamp)
    d.text((W - 24, cy), stamp, font=f_stamp, fill=TEXT_SOFT, anchor="rm")
    f_project = font_for(job.get("project", "agent"), S_PROJECT)
    d.text((cx + r + 14, cy), fit(job.get("project", "agent"), f_project,
                                  W - 24 - stamp_w - 24 - (cx + r + 14)),
           font=f_project, fill=TEXT, anchor="lm")

    # State label -- the one thing that must be readable across the room, and the only
    # place colour is spent.
    y = 84
    f_label = font_for(label, S_LABEL)
    d.text((28, y), fit(label, f_label, W - 56), font=f_label, fill=color)
    y += S_LABEL + 24

    # With a picture up, the path and the "Calling x" line are noise -- the caption
    # under the picture already names the file, and the room is better spent on the
    # picture itself. The header and the state label stay: they are what tells the
    # user *why* a picture is on the panel.
    picture = job.get("image")
    if not picture:
        if job.get("target"):
            f = font_for(job["target"], S_TARGET, mono=True)
            d.text((28, y), fit(job["target"], f, W - 56), font=f, fill=TEXT_SOFT)
            y += S_TARGET + 12
        if job.get("detail"):
            f = font_for(job["detail"], S_DETAIL)
            d.text((28, y), fit(job["detail"], f, W - 56), font=f, fill=TEXT)
            y += S_DETAIL + 18

    if picture:
        sy = y + 4
        d.line((28, sy, W - 28, sy), fill=BORDER, width=1)
        used, dims, err = draw_image_preview(img, d, picture, sy + 18)
        bits = [os.path.basename(str(picture)) or str(picture)]
        if dims:
            bits.append(dims)
        left = job.get("imageLeft")
        if isinstance(left, int) and left > 0:
            bits.append(f"{left}s")     # auto-clears; say when
        f_cap = font(S_FOOTER)
        d.text((28, sy + 18 + used + 12), fit(" · ".join(bits), f_cap, W - 56),
               font=f_cap, fill=ERR if err else TEXT_SOFT)
        return

    progress = job.get("progress")
    if progress is not None:
        frac = max(0.0, min(1.0, float(progress)))
        bar_w = W - 56 - 74          # leave room for the percentage
        d.rounded_rectangle((28, y, 28 + bar_w, y + 14), radius=7,
                            fill=BG, outline=BORDER, width=1)
        if frac > 0:
            d.rounded_rectangle((28, y, 28 + max(int(bar_w * frac), 14), y + 14),
                                radius=7, fill=color)
        d.text((W - 28, y + 7), f"{frac * 100:.0f}%", font=font(S_PROGRESS, True),
               fill=TEXT_SOFT, anchor="rm")
        y += 34

    # Log tail. It yields to the buttons: those sit at H-100, so a log that ran
    # past them would be covered.
    log = job.get("log") or []
    f_log = font(S_LOG)
    step = S_LOG + 14
    buttons = job.get("buttons") or []
    floor = (H - 100 - 14) if buttons else (H - 22)

    if log:
        sy = y + 6
        d.line((28, sy, W - 28, sy), fill=BORDER, width=1)
        ly = sy + 18
        room = (floor - ly) // step
        n = max(0, min(len(log), room))
        for entry in (log[-n:] if n else []):
            kind, text, _tool = parse_log(entry)
            # Colour on the bullet only -- a failed step is worth spotting in the tail,
            # everything else stays one shade. No icons: they were tried and rejected.
            d.text((28, ly), "·", font=f_log,
                   fill=ERR if kind == "error" else ACCENT)
            d.text((48, ly), fit(text, f_log, W - 48 - 28), font=f_log,
                   fill=TEXT_SOFT)
            ly += step


def draw_buttons(d, job):
    """Optional tap targets on the status page (unused while a question is up)."""
    buttons = job.get("buttons") or []
    hits = []
    if not buttons:
        return hits
    state = job.get("state", "idle")
    color = STATE_COLORS.get(state, ACCENT)
    n = len(buttons)
    gap = 12
    bh = 72
    by = H - 28 - bh
    bw = (W - 56 - gap * (n - 1)) // n
    for i, b in enumerate(buttons):
        bx = 28 + i * (bw + gap)
        first = (i == 0)
        d.rounded_rectangle((bx, by, bx + bw, by + bh), radius=14, fill=BG,
                            outline=color if first else BORDER, width=2 if first else 1)
        f = font_for(b, 26)
        d.text((bx + bw // 2, by + bh // 2), fit(b, f, bw - 16), font=f,
               fill=color if first else TEXT, anchor="mm")
        hits.append({"x": bx, "y": by, "w": bw, "h": bh, "value": str(b)})
    return hits


def render_status(job):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    draw_status(d, img, job)
    # A picture owns the lower half; tap targets belong to questions, not pictures.
    hits = [] if job.get("image") else draw_buttons(d, job)
    return img, hits


def render_image(job):
    """Deprecated alias -- the picture is now a preview *inside* the status page.

    It used to take over the whole panel, which hid the very information the panel
    exists to show, and it stayed up until the next tool call. Keeping the alias means
    an older caller (or a stale page name) still draws something sensible instead of
    silently falling back to a bare status page.
    """
    return render_status(job)


def render_ask(job):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, W, 4), fill=WARN)
    d.text((28, 26), "需要你决定", font=font(S_ASK_LABEL), fill=WARN)

    q = str(job.get("question", ""))
    f_q = font_for(q, S_ASK_Q)
    lh = S_ASK_Q + 12
    q_top = 72
    all_lines = wrap(q, f_q, W - 56)

    opts = [str(o) for o in (job.get("options") or [])][:5]
    n = max(1, len(opts))
    gap = 12
    footer = job.get("footer")
    footer_h = 36 if footer else 12

    # Work the layout out backwards from the bottom edge: the buttons are what the
    # user has to hit, so they claim their space first and the question gets the rest.
    #
    # The floor is 44px, not 52: with five options the difference is one more line of
    # the *question*, and the question is the thing the user must actually read to
    # answer. A 44px row still takes a finger.
    bottom = H - footer_h
    opt_top = q_top + lh + 18
    bh = 0
    for take in range(min(len(all_lines), 5), 0, -1):
        top = max(q_top + take * lh + 18, opt_top)
        room = (bottom - top) - gap * (n - 1)
        if room // n >= 44:
            opt_top, bh = top, min(84, room // n)
            break
    if bh == 0:                      # very long question + many options: shrink
        opt_top = q_top + lh + 18
        room = (bottom - opt_top) - gap * (n - 1)
        bh = max(40, min(84, room // n))
    lines = all_lines[:max(1, (opt_top - q_top - 18) // lh)]
    if len(lines) < len(all_lines):
        lines[-1] = fit(lines[-1] + "…", f_q, W - 56)

    y = q_top
    for ln in lines:
        d.text((28, y), ln, font=f_q, fill=TEXT)
        y += lh

    hits = []
    opt_size = S_ASK_OPT if bh >= 56 else S_ASK_OPT_SMALL
    f_opt = font(opt_size)
    f_num = font(opt_size, True)
    for i, opt in enumerate(opts):
        by = opt_top + i * (bh + gap)
        d.rounded_rectangle((28, by, W - 28, by + bh), radius=14, fill=BG,
                            outline=ACCENT if i == 0 else BORDER,
                            width=2 if i == 0 else 1)
        mid = by + bh // 2
        d.text((56, mid), str(i + 1), font=f_num, fill=ACCENT, anchor="lm")
        d.text((94, mid), fit(opt, f_opt, W - 94 - 32), font=f_opt, fill=TEXT,
               anchor="lm")
        hits.append({"x": 28, "y": by, "w": W - 56, "h": bh, "value": str(i)})

    if footer:
        d.text((28, H - 12), fit(footer, font(S_FOOTER), W - 56),
               font=font(S_FOOTER), fill=TEXT_SOFT, anchor="ls")
    return img, hits


def render_boot(job):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((W // 2, H // 2 - 30), "mosaico-deck", font=font(S_BOOT), fill=TEXT,
           anchor="mm")
    d.text((W // 2, H // 2 + 28), "等待 Harness…", font=font(S_ASK_LABEL),
           fill=TEXT_SOFT, anchor="mm")
    return img, []


PAGES = {"status": render_status, "image": render_image, "ask": render_ask,
         "boot": render_boot}


def main():
    line = sys.stdin.readline()
    if not line:
        return 1
    job = json.loads(line)
    img, hits = PAGES.get(job.get("page", "status"), render_status)(job)

    if job.get("hits"):
        # The caller wants button positions: send the hit regions over a
        # separate stderr side channel (stdout carries only pixels; structured
        # data cannot be mixed into a binary stream).
        sys.stderr.write(json.dumps(hits) + "\n")
        sys.stderr.flush()

    # stdout must be clean raw pixels -- so all output above goes to stderr.
    sys.stdout.buffer.write(img.tobytes())
    sys.stdout.buffer.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
