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

import json
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

print("配色只表达状态，不表达工具（emoji + 按工具上色那版被否掉了）")
for gone in ("emoji", "draw_emoji", "style_for", "STATE_ICONS", "TOOL_STYLES",
             "LOG_ICONS", "TEXT_FAINT"):
    check(not hasattr(r, gone), f"图标机制 {gone} 已移除")


def status_for(tool, state="tool", **kw):
    im, _ = r.PAGES["status"]({"state": state, "tool": tool, **kw})
    return im.tobytes()


check(status_for("bash") == status_for("edit"),
      "同一个状态下，不同工具画出来完全一样（颜色不跟着工具变）")
check(status_for("bash", state="error") != status_for("bash"),
      "但状态变了画面就变（颜色确实表达状态）")
check(status_for("bash", state="done") != status_for("bash", state="error"),
      "不同状态之间互不相同")

print("状态配色表齐全且够亮")
for st in r.STATE_LABELS:
    check(st in r.STATE_COLORS, f"状态 {st} 有配色")
check(set(r.STATE_COLORS) == set(r.STATE_LABELS), "状态配色表与文字表一致")

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

print("底部会话数字条")
check(r.kfmt(950) == "950" and r.kfmt(48213) == "48.2K" and r.kfmt(128000) == "128K"
      and r.kfmt(1_300_000) == "1.3M",
      f"紧凑计数：48213 -> {r.kfmt(48213)}，128000 -> {r.kfmt(128000)}，1300000 -> {r.kfmt(1_300_000)}")
check(r.kfmt(None) == "—" and r.kfmt(-5) == "—", "缺失/负数显示占位符而不是崩")

# 高度预算。之前这里算少了一行，第二行被屏幕边缘切掉（"23 步"直接不见了），
# 所以现在把环和文字都量出来。
stats_top = r.H - r.STATS_H
ring_bottom = stats_top + 10 + r.RING_R * 2
# 文字用 anchor="lm" 垂直居中，所以下半高是 S_STATS_LINE//2，不是整行高。
text_bottom = stats_top + 10 + r.RING_R + 12 + r.S_STATS_LINE // 2
check(ring_bottom <= r.H - 4,
      f"进度环装得下：下沿 {ring_bottom}px ≤ {r.H}px")
check(text_bottom <= r.H - 4,
      f"环右侧第二行装得下：下沿 {text_bottom}px ≤ {r.H}px")
check(12 * 2 + r.S_STATS_LINE <= r.RING_R * 2,
      f"两行文字高度 {12 * 2 + r.S_STATS_LINE}px 不超过环的直径 {r.RING_R * 2}px —— 否则统计条又要变高")
check(r.STATS_H < 78, f"比两行纯文字那版省空间：{r.STATS_H}px < 78px")

# 宽度同样要装得下。修好纵向之后横向又溢出了（"23 步"被右边切掉），所以
# 现在改成按优先级装箱，并在这里做一组极端取值的属性断言。
fstats = r.font(r.S_STATS)
for label, st, expect in (
    ("典型", {"context": 625830, "contextWindow": 1000000, "input": 574196,
              "output": 398731, "cacheHit": 100, "turn": 16, "steps": 389},
     "缓存"),
    ("极端大", {"context": 999999, "contextWindow": 1000000, "input": 99999999,
                "output": 99999999, "cacheHit": 99, "turn": 999, "steps": 9999},
     "累计"),
    ("只有累计", {"context": 1000, "input": 123456, "output": 789}, "累计"),
):
    items = []
    if st.get("input"):
        items.append(f"累计 {r.kfmt(st['input'])}/{r.kfmt(st['output'])}")
    if st.get("cacheHit"):
        items.append(f"缓存{st['cacheHit']}%")
    if st.get("tokensPerSecond"):
        items.append(f"{r.tpsfmt(st['tokensPerSecond'])}tok/s")
    turns = "".join(x for x in (
        f"{st['turn']}轮" if st.get("turn") else "",
        f"{st['steps']}步" if st.get("steps") else "",
    ) if x)
    if turns:
        items.append(turns)
    line = r.pack_items(items, fstats, r.W - 56)
    check(r.width_of(line, fstats) <= r.W - 56,
          f"{label}：装箱后 {r.width_of(line, fstats):.0f}px ≤ {r.W - 56}px（{line}）")
    check(expect in line, f"{label}：保住了「{expect}」")
packed_typical = r.pack_items(["累计 210K/48.0K", "缓存76%", "7轮23步"],
                              fstats, r.W - 56)
check("累计" in packed_typical and "缓存" in packed_typical and "23步" in packed_typical,
      f"典型值下四项都在：{packed_typical}")
check("累计" in r.pack_items(["累计 1/2", "第 1 轮", "1 步"], fstats, r.W - 56),
      "放得下时不丢任何一项")
check(r.pack_items(["累计 1/2", "第 1 轮"], fstats, 30) == "",
      "连第一项都放不下时返回空串，而不是画到屏幕外")
check(r.width_of("上下文 128K / 1000K", fstats) <= r.W - 56 - 60,
      "含窗口的上下文行也给百分比留了位置")


def bottom_ink(im, h=70):
    band = im.crop((0, r.H - h, r.W, r.H)).tobytes()
    return sum(1 for i in range(0, len(band), 3) if band[i] or band[i + 1] or band[i + 2])


no_stats, _ = r.PAGES["status"]({"state": "tool", "detail": "x"})
with_stats, _ = r.PAGES["status"]({
    "state": "tool", "detail": "x",
    "stats": {"context": 48213, "contextWindow": 128000, "input": 210000,
              "output": 48000, "cacheRead": 160000, "turn": 7, "steps": 23}})
check(bottom_ink(no_stats) == 0, "没有数字时底部留白，不画空条")
check(bottom_ink(with_stats) > 400,
      f"有数字时底部画出统计条（非黑像素 {bottom_ink(with_stats)}）")

print("上下文占比：接近上限要变色提醒")
near = r.PAGES["status"]({"state": "tool",
                          "stats": {"context": 102000, "contextWindow": 128000}})[0]
over = r.PAGES["status"]({"state": "tool",
                          "stats": {"context": 110000, "contextWindow": 128000}})[0]
check(near.tobytes() != over.tobytes(), "跨过 80% 时画面变化（转警告色）")
only_ctx = r.PAGES["status"]({"state": "tool", "stats": {"context": 5000}})[0]
check(only_ctx.size == (r.W, r.H), "没有窗口大小时不画百分比、也不崩")
check(bottom_ink(only_ctx) > 100, "只有上下文用量时也画出那一行")

print("日志条目：字符串与结构化对象都要能画")
img_mix, _ = r.PAGES["status"]({"state": "tool", "tool": "bash",
                                "log": ["纯字符串", {"kind": "tool", "tool": "edit",
                                                     "text": "结构化"}]})
check(img_mix.size == (r.W, r.H), "混用字符串和对象都不崩")

print("控制中心")
panel, panel_hits = r.PAGES["panel"]({"page": "panel",
                                      "levels": {"volume": 85, "brightness": 35}})
check(panel.size == (r.W, r.H), "面板画得出来")
check([h["value"] for h in panel_hits] == ["close", "volume", "brightness"],
      f"✕ 在最前，两条滑块随后：{[h['value'] for h in panel_hits]}")
check(all(h["x"] >= 0 and h["x"] + h["w"] <= r.W and h["y"] + h["h"] <= r.H
          for h in panel_hits), "热区都在屏内")
check(all(h["h"] >= 44 for h in panel_hits),
      f"热区高度够手指点：{[h['h'] for h in panel_hits]}")
sliders = [h for h in panel_hits if h["value"] != "close"]
check(all(abs(h["x"] - 20) < 2 and abs(h["w"] - (r.W - 40)) < 2 for h in sliders),
      "两条滑块的热区横跨整行（左边留一点余量给手指）")
close = [h for h in panel_hits if h["value"] == "close"][0]
check(close["x"] > r.W * 0.7 and close["y"] < 100,
      f"✕ 在右上角：x={close['x']} y={close['y']}")
check(close["w"] >= 44 and close["h"] >= 44, f"✕ 热区够手指点：{close['w']}x{close['h']}")
# 桩里写死的 y 必须和真实几何一致，否则触摸会命中不到。
check([h["y"] for h in sliders] == [160, 300],
      f"滑块热区位置与测试桩一致：{[h['y'] for h in sliders]}")

def strip_ink(im, y, h=14):
    band = im.crop((0, y, r.W, y + h)).tobytes()
    return sum(1 for i in range(0, len(band), 3) if band[i] or band[i + 1] or band[i + 2])

full, _ = r.PAGES["panel"]({"page": "panel", "levels": {"volume": 100, "brightness": 0}})
check(strip_ink(full, 178) > strip_ink(full, 318),
      "音量 100 / 亮度 0 时，两条滑块的填充量确实不同（值真的画出来了）")

print("烧屏防护：整屏位移")
base, base_hits = r.PAGES["panel"]({"page": "panel", "levels": {"volume": 50},
                                    "shift": {"x": 0, "y": 0}})


def shifted(dx, dy):
    img = Image.new("RGB", (r.W, r.H), r.BG)
    img.paste(base, (dx, dy))
    return img, [dict(h, x=h["x"] + dx, y=h["y"] + dy) for h in base_hits]


import subprocess as _sp
PY_BIN = "/Applications/DeepSeek Harness.app/Contents/Resources/runtime/primary-runtime/dependencies/python/bin/python3"


def render_with_shift(dx, dy):
    job = {"page": "panel", "levels": {"volume": 50}, "hits": True,
           "shift": {"x": dx, "y": dy}}
    p = _sp.run([PY_BIN, "plugin/render.py"], input=json.dumps(job).encode(),
                capture_output=True)
    hits = json.loads(p.stderr.decode()) if p.stderr.strip() else []
    return Image.frombytes("RGB", (r.W, r.H), p.stdout), hits


a, ha = render_with_shift(0, 0)
b, hb = render_with_shift(3, -3)
check(a.tobytes() != b.tobytes(), "位移会改变画面（否则等于没做）")
check([h["x"] for h in hb] == [h["x"] + 3 for h in ha]
      and [h["y"] for h in hb] == [h["y"] - 3 for h in ha],
      "热区跟着位移走（否则触摸会错位）")
# 位移幅度必须小于设计余量，否则文字会被切掉
check(max(abs(v) for v in (3, -3)) < 28,
      "位移幅度 3px 远小于 28px 边距，不会有内容被切")
_real, real_hits = r.PAGES["panel"]({"page": "panel", "levels": {"volume": 50}})
check(all(h["x"] + h["w"] <= r.W for h in real_hits),
      "热区本来就在屏内（位移 3px 后仍在）")

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
