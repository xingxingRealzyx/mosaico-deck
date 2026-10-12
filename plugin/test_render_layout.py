#!/usr/bin/env python3
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# test_render_layout.py -- the two panel defects, pinned down without hardware.
#
#   <dsh python> plugin/test_render_layout.py
#
# Both bugs here were invisible in the code and obvious on the panel:
#
#   1. The ask page floored its option height at 52px, so with five options the last
#      row was drawn *below* the 480px panel while its tap rectangle stayed live, and
#      the row before it overlapped the footer.
#   2. `target` was drawn with the monospace face, which has no CJK glyphs -- a path
#      containing Chinese lost those characters entirely (blank gaps, stray slashes).
#
# Neither needs a board to catch: the tap rectangles come back over stderr, and the
# font choice is a pure function.

import importlib.util
import pathlib
import sys

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


print("字体选择")
mono = r.font(20, True)
check(r.font_for("中文", 20, mono=True) is not mono,
      "含中文的字符串不会用等宽字体（等宽字体没有 CJK 字形）")
check(r.font_for("/dev/cu.usbmodem111203", 20, mono=True) is mono,
      "纯 ASCII 仍然用等宽字体（路径对齐好看）")

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

print("状态页：中英混排的目标路径能画出字形（不是空白）")
job = {"page": "status", "state": "editing", "detail": "改代码",
       "target": "/Users/x/中文目录/device.js", "log": ["一行", "两行"]}
img, hits = r.PAGES["status"](job)
# 中文只出现在 target 那一行：若字体回退失败，那一段就是空白，
# 该行的非黑像素会明显偏少。
y0, y1 = 150, 200
band = img.crop((0, y0, r.W, y1)).tobytes()
ink = sum(1 for i in range(0, len(band), 3) if band[i] or band[i + 1] or band[i + 2])
check(ink > 800, f"目标路径那一行有实际字形（非黑像素 {ink}）")

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
print("[ok] 排版与字体选择符合契约")
