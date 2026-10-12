#!/usr/bin/env python3
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# test_render_layout.py -- the panel defects, pinned down without hardware.
#
#   <dsh python> plugin/test_render_layout.py
#
# Everything here was invisible in the code and obvious on the panel:
#
#   1. The ask page floored its option height at 52px, so with five options the last
#      row was drawn *below* the 480px panel while its tap rectangle stayed live, and
#      the row before it overlapped the footer.
#   2. `target` was drawn with the monospace face, which has no CJK glyphs -- a path
#      containing Chinese lost those characters entirely (blank gaps, stray slashes).
#   3. Dim grey on black is not readable on this panel, at this size, at desk
#      distance. The fix was to stop expressing hierarchy by dimming, and to drop the
#      thin faces.
#
# None of it needs a board: the tap rectangles come back over stderr, and the font
# and palette are pure functions.

import importlib.util
import os
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("deck_render", HERE / "render.py")
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)

failures = 0


def check(ok, msg):
    global failures
    if not ok:
        failures += 1
    print(f"  [{'ok' if ok else 'FAIL'}] {msg}")


def ink(text, size=28):
    """Non-black pixel count for one string drawn in the UI face."""
    return ink_with(r.font_for(text, size), text)


def ink_with(f, text):
    """Non-black pixel count for one string in one specific font object."""
    im = Image.new("RGB", (300, 64), (0, 0, 0))
    ImageDraw.Draw(im).text((4, 8), text, font=f, fill=r.TEXT)
    raw = im.tobytes()
    return sum(1 for i in range(0, len(raw), 3) if raw[i] or raw[i + 1] or raw[i + 2])


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


print("字体：一个字面，且必须画得出字形")
check(ink("调用工具") > 400, "中文画得出实际字形（曾经的等宽字面没有 CJK 字形）")
check(ink("device.js") > 250, "ASCII 同样画得出")
check(r.font_for("中文", 20) is r.font(20),
      "不再按内容切换字体：只有一个字面")

# 字重不能靠肉眼挑：Hiragino Sans GB.ttc 的 index 1 看着比 index 0 粗，
# 实测反而更细（它是 W3 的 UI 变体）。所以拿 W3 当基准量墨量。
w3_path = "/System/Library/Fonts/Hiragino Sans GB.ttc"
if os.path.exists(w3_path):
    w3 = ImageFont.truetype(w3_path, 40, index=0)
    chosen = r.font(40)
    a, b = ink_with(w3, "调用工具"), ink_with(chosen, "调用工具")
    check(b >= a * 1.2,
          f"所选字面比 W3 至少粗 20%（W3={a} 所选={b}，{b / a:.2f}x）")

print("配色：黑底上的文字必须够亮")
check(lum(r.TEXT) > 240, f"主文字接近白 (亮度 {lum(r.TEXT):.0f})")
check(lum(r.TEXT_SOFT) > 200,
      f"次级文字仍要亮——压暗正是之前看不清的原因 (亮度 {lum(r.TEXT_SOFT):.0f})")
check(lum(r.BG) == 0, "背景是纯黑")
for name in ("ACCENT", "OK", "WARN", "ERR", "IDLE"):
    c = getattr(r, name)
    check(lum(c) > 130, f"{name} 是亮色 (亮度 {lum(c):.0f})")
check(not hasattr(r, "TEXT_FAINT"),
      "没有更暗的第三档灰色了（层级靠字号和字重，不靠压暗）")

print("ask 页布局：任何问题长度 x 任何选项数都必须全部落在屏内")
for qlen in (4, 40, 90, 160):
    for n_opts in (2, 3, 5):
        job = {"page": "ask", "question": "问" * qlen,
               "options": [f"选项{i + 1}" for i in range(n_opts)],
               "footer": "按住 AI 键取消", "hits": True}
        img, hits = r.PAGES["ask"](job)
        oob = [h for h in hits
               if h["y"] < 0 or h["y"] + h["h"] > r.H or h["x"] < 0 or h["x"] + h["w"] > r.W]
        rows = sorted(hits, key=lambda h: h["y"])
        overlap = any(rows[i]["y"] + rows[i]["h"] > rows[i + 1]["y"]
                      for i in range(len(rows) - 1))
        tag = f"问题 {qlen} 字 / {n_opts} 个选项"
        check(len(hits) == n_opts, f"{tag}: 每个选项都有热区")
        check(not oob, f"{tag}: 热区全部在 480px 屏内")
        check(not overlap, f"{tag}: 相邻选项不重叠")

print("状态页：中英混排的目标路径必须画出字形（不是空白）")
job = {"page": "status", "state": "editing", "detail": "改代码",
       "target": "/Users/x/中文目录/device.js", "log": ["一行", "两行"]}
img, hits = r.PAGES["status"](job)
# 目标路径那一行：字体回退失败的话那一段就是空白，非黑像素会明显偏少。
band = img.crop((0, 152, r.W, 186)).tobytes()
n_ink = sum(1 for i in range(0, len(band), 3) if band[i] or band[i + 1] or band[i + 2])
check(n_ink > 1500, f"目标路径那一行有实际字形（非黑像素 {n_ink}）")

print("彩色 emoji 图标")
tile = r.emoji("🖥", 28)
check(tile is not None and tile.height == 28, "emoji 渲染成 28px 高的图块")
check(r.emoji("", 28) is None, "空字符返回 None 而不是抛异常")


def spread_of(ch, px=28):
    t = r.emoji(ch, px)
    if t is None:
        return 0
    raw = t.convert("RGB").tobytes()
    return max(max(raw[i:i + 3]) - min(raw[i:i + 3]) for i in range(0, len(raw), 3))


# 挑本身高饱和的 emoji：🖥 是银灰色显示器，通道差本来就小，拿它验"彩色"会误判。
sat = {ch: spread_of(ch) for ch in ("✅", "🎁", "📖")}
check(max(sat.values()) > 60,
      f"emoji 带真实色彩而不是灰度字形（{'  '.join(f'{k}={v}' for k, v in sat.items())}）")

print("工具配色：每个工具家族有自己的图标和颜色")
for name, want in (("bash", r.OK), ("read", r.BLUE), ("edit", r.WARN),
                   ("web_search", r.CYAN), ("subagent", r.MAGENTA)):
    icon, color = r.style_for(name)
    check(color == want and bool(icon), f"{name} -> {icon} 颜色正确")
check(r.style_for("read_image")[0] != r.style_for("read")[0],
      "read_image 与 read 图标不同（按最长键匹配）")
hues = {r.style_for(t)[1] for t in
        ("bash", "read", "edit", "glob", "web_search", "subagent", "job")}
check(len(hues) >= 6, f"至少 6 种不同颜色（实际 {len(hues)}）")
check(r.style_for("完全没见过的工具")[1] == r.ACCENT, "未知工具回退到默认色")
for st in r.STATE_LABELS:
    check(bool(r.STATE_ICONS.get(st)), f"状态 {st} 有对应 emoji")

print("图片预览：画在状态页里，**不整屏接管**")
tmp_img = "/tmp/_deck_test_image.png"
# 纯白图：这样"图片画到了哪一行"可以直接用白色像素数判断。
Image.new("RGB", (400, 400), (255, 255, 255)).save(tmp_img)


def white_run(im, y):
    row = im.crop((0, y, r.W, y + 1)).tobytes()
    return sum(1 for i in range(0, len(row), 3)
               if row[i] > 200 and row[i + 1] > 200 and row[i + 2] > 200)


img, hits = r.PAGES["status"]({"page": "status", "image": tmp_img,
                               "state": "tool", "tool": "read_image", "imageLeft": 7})
check(white_run(img, 300) > 200,
      f"图片画在下方的预览区（y=300 处白色像素 {white_run(img, 300)}）")
# 这条是"太大"那个问题的回归测试：整屏接管时这一行会被图片铺满。
check(white_run(img, 100) < 100,
      f"状态标签那一行仍归状态用，没被图片占满（y=100 处白色像素 {white_run(img, 100)}）")
check(hits == [], "图片模式没有热区，点屏幕不会误触")
check(r.IMG_MAX_H <= 300, f"预览高度上限 {r.IMG_MAX_H}px，不是整屏 480")

print("倒计时必须真的画出来（用户要知道它什么时候消失）")
img9, _ = r.PAGES["status"]({"page": "status", "image": tmp_img,
                             "state": "tool", "imageLeft": 9})
img3, _ = r.PAGES["status"]({"page": "status", "image": tmp_img,
                             "state": "tool", "imageLeft": 3})
check(img9.tobytes() != img3.tobytes(), "倒计时数字变化会改变画面")

print("坏图片路径")
img_bad, _ = r.PAGES["status"]({"page": "status", "image": "/nope/missing.png",
                                "state": "tool"})
check(img_bad.size == (r.W, r.H), "文件读不出来时不抛异常")
check(any(img_bad.crop((0, 180, r.W, 240)).tobytes()), "坏路径显示一行错误说明")

print("旧的 image 页名仍然可用（现在等同于状态页 + 预览）")
img_alias, _ = r.PAGES["image"]({"page": "image", "image": tmp_img, "state": "tool"})
img_new, _ = r.PAGES["status"]({"page": "status", "image": tmp_img, "state": "tool"})
check(img_alias.tobytes() == img_new.tobytes(), "旧页名不会退化成一张空白状态页")

print("日志条目：字符串与结构化对象都要能画")
img_mix, _ = r.PAGES["status"]({"state": "tool", "tool": "bash",
                                "log": ["纯字符串", {"kind": "tool", "tool": "edit",
                                                     "text": "结构化"}]})
check(img_mix.size == (r.W, r.H), "混用字符串和对象都不崩")

print("纯黑背景")
for page, j in (("status", {"state": "tool", "detail": "x", "log": ["a"]}),
                ("ask", {"question": "短问题", "options": ["是", "否"]}),
                ("boot", {})):
    j = dict(j, page=page)
    img, _ = r.PAGES[page](j)
    corners = [img.getpixel(p) for p in ((2, 470), (470, 470), (470, 100), (2, 100))]
    check(all(c == (0, 0, 0) for c in corners), f"{page} 页四角是纯黑 {corners[0]}")

print()
if failures:
    print(f"[FAIL] {failures} 项未通过")
    sys.exit(1)
print("[ok] 排版、字体与配色符合契约")
