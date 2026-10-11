#!/usr/bin/env python3
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# bridge.py —— mosaico-deck 的 PC 侧桥
#
# 它做两件事：
#   1. 把"agent 的状态"渲染成 480×480，通过 USB 送给板子
#   2. 把板子上的触摸/按键转成结构化的回答，交回给 agent
#
# 对外是 **MCP server**（stdio），所以任何支持 MCP 的 agent 都能用，不用为
# 某一个 agent 单独适配。配置大概是：
#
#     "mcpServers": {
#       "mosaico-deck": {
#         "command": "/path/to/mosaico-deck/.venv/bin/python",
#         "args": ["/path/to/mosaico-deck/tools/bridge/bridge.py"]
#       }
#     }
#
# 三种运行模式：
#     bridge.py                # 默认：MCP server（stdio），给 agent 用
#     bridge.py --demo         # 跑一段假的 agent 流程，看屏幕上的真实效果
#     bridge.py --preview      # 只把页面存成 PNG，不连板子（调界面用）

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import ui  # noqa: E402
from deck import Deck, find_port  # noqa: E402


# ---------------------------------------------------------------------------
# 状态
# ---------------------------------------------------------------------------
@dataclass
class DeckState:
    project: str = "agent"
    state: str = "idle"
    detail: str = ""
    target: str = ""
    progress: float | None = None
    started_at: float = field(default_factory=time.time)
    files_changed: int = 0
    tests_passed: int = 0
    tests_total: int = 0
    log: list[str] = field(default_factory=list)

    def elapsed(self) -> float:
        return time.time() - self.started_at


# ---------------------------------------------------------------------------
# 桥
# ---------------------------------------------------------------------------
class Bridge:
    """把一块板子变成"可被 agent 调用的界面"。

    线程模型：
      · 串口事件在 Deck 的接收线程里来 → 用 call_soon_threadsafe 丢回 asyncio
      · UI 渲染和串口写都在 asyncio 线程里做（简单，而且这点量完全够）
    """

    def __init__(self, port: str):
        self.deck = Deck(port, on_event=self._on_deck_event)
        self.state = DeckState()
        self.buttons: list[ui.Button] = []
        self.pending: asyncio.Future | None = None
        self.loop: asyncio.AbstractEventLoop | None = None
        self._lock = threading.Lock()
        self._dirty = False
        self.ask_options: list[str] = []
        self.auto_answer: int | None = None

    # ---- 生命周期 ----------------------------------------------------------
    async def start(self) -> None:
        self.loop = asyncio.get_running_loop()
        self.deck.open(wait_ready=5.0)
        await self.redraw(force_full=True)

    def stop(self) -> None:
        self.deck.close()

    # ---- 板子 → PC ---------------------------------------------------------
    def _on_deck_event(self, ev: dict) -> None:
        """在 Deck 的接收线程里被调用。**不能直接碰 asyncio 对象。**"""
        if self.loop is None:
            return
        self.loop.call_soon_threadsafe(self._handle_event, ev)

    def _handle_event(self, ev: dict) -> None:
        kind = ev.get("t")
        if kind == "touch":
            if ev.get("ev") == "down":
                self._on_tap(int(ev.get("x", 0)), int(ev.get("y", 0)))
        elif kind == "key":
            # AI 按键 = 取消当前问询。物理按键比在屏幕上找"取消"更顺手。
            if self.pending is not None and not self.pending.done():
                self.pending.set_result(None)
        elif kind == "ack":
            pass

    def _on_tap(self, x: int, y: int) -> None:
        for b in self.buttons:
            if b.hit(x, y):
                print(f"[bridge] 点中按钮 {b.value} ({x},{y})", file=sys.stderr)
                if self.pending is not None and not self.pending.done():
                    self.pending.set_result(b.value)
                return
        print(f"[bridge] 点在空白处 ({x},{y})", file=sys.stderr)

    # ---- 渲染 --------------------------------------------------------------
    async def redraw(self, force_full: bool = False) -> int:
        """按当前状态重画并按需发送。返回发出的 band 数。"""
        with self._lock:
            img, self.buttons = ui.render_status(
                project=self.state.project,
                state=self.state.state,
                detail=self.state.detail,
                target=self.state.target,
                progress=self.state.progress,
                elapsed_s=self.state.elapsed(),
                files_changed=self.state.files_changed,
                tests_passed=self.state.tests_passed,
                tests_total=self.state.tests_total,
                log=self.state.log,
                buttons=[("批准", "approve"), ("继续", "continue")]
                if self.state.state == "waiting"
                else None,
            )
        n = await asyncio.to_thread(self.deck.send_image, img, force_full)
        return n

    async def show_ask(self, question: str, options: list[str]) -> None:
        with self._lock:
            img, self.buttons = ui.render_ask(question, options)
        self.ask_options = list(options)
        await asyncio.to_thread(self.deck.send_image, img, True)

    def tap_button(self, index: int) -> bool:
        """用代码模拟一次点击（按按钮中心）。

        存在的意义：**没有人在场也能验证整条链路**（渲染 -> 等待 -> 点击 -> 回答）。
        手动测试要点屏幕，自动化测试点不了。--auto-answer 用的就是它。
        """
        if not (0 <= index < len(self.buttons)):
            return False
        b = self.buttons[index]
        self._on_tap(b.x + b.w // 2, b.y + b.h // 2)
        return True

    # ---- 对外的三个动作 ----------------------------------------------------
    async def display_status(self, **kw) -> str:
        if "project" in kw and kw["project"]:
            self.state.project = str(kw["project"])
        if "state" in kw and kw["state"]:
            new_state = str(kw["state"])
            # 状态从"非运行"切到"运行"时重置计时器和日志 —— 否则上一轮的
            # 耗时和日志会串到这一轮，看着很怪。
            if new_state != self.state.state and new_state in ("thinking", "running",
                                                               "editing", "testing"):
                if self.state.state in ("idle", "done", "error", "failed"):
                    self.state.started_at = time.time()
                    self.state.log = []
            self.state.state = new_state
        for k in ("detail", "target"):
            if k in kw and kw[k] is not None:
                setattr(self.state, k, str(kw[k]))
        if "progress" in kw and kw["progress"] is not None:
            self.state.progress = float(kw["progress"])
        if "files_changed" in kw and kw["files_changed"] is not None:
            self.state.files_changed = int(kw["files_changed"])
        if "tests_passed" in kw and kw["tests_passed"] is not None:
            self.state.tests_passed = int(kw["tests_passed"])
        if "tests_total" in kw and kw["tests_total"] is not None:
            self.state.tests_total = int(kw["tests_total"])
        if "log_line" in kw and kw["log_line"]:
            self.state.log.append(str(kw["log_line"]))
            self.state.log = self.state.log[-6:]

        n = await self.redraw()
        return f"已更新（{n} 条 band）" if n else "内容没变，没发"

    async def ask_user(self, question: str, options: list[str], timeout_s: float) -> str:
        """在设备上提问并**阻塞等待**人回答。

        返回选项文本；超时返回 "timeout"；按了 AI 键取消返回 "cancelled"。
        这两个特殊值要让 agent 知道，而不是伪装成一个普通选项。
        """
        if not options:
            return "cancelled"

        self.pending = self.loop.create_future() if self.loop else asyncio.get_running_loop().create_future()
        await self.show_ask(question, options)
        try:
            value = await asyncio.wait_for(self.pending, timeout=timeout_s)
        except asyncio.TimeoutError:
            value = None
        finally:
            pending, self.pending = self.pending, None
            if pending is not None and not pending.done():
                pending.cancel()
            # 回到状态页 —— 问完就把屏幕交还，否则会一直停在问询页
            await self.redraw(force_full=True)

        if value is None:
            return "timeout"
        try:
            idx = int(value)
        except (TypeError, ValueError):
            return str(value)
        if 0 <= idx < len(options):
            return options[idx]
        return str(value)

    async def notify(self, text: str, state: str = "waiting") -> str:
        """不阻塞地提示一下（状态页上加一行 + 状态变色）。"""
        self.state.log.append(text)
        self.state.log = self.state.log[-6:]
        self.state.state = state
        n = await self.redraw()
        return f"已提示（{n} 条 band）"


# ---------------------------------------------------------------------------
# MCP server
# ---------------------------------------------------------------------------
# 用 mcp SDK 的**高层接口** MCPServer（2.x 里 FastMCP 改名成了它）。
# 好处是 schema 直接从类型注解生成，不用手写 JSON Schema —— 少一处会和
# 实现脱节的地方。
#
# 注意工具参数都给了默认值：agent 调 display_status 时通常只想改一两个字段
#（比如只追一行 log_line），其余保持原样。全部 required 会逼它每次把整个状态
# 重发一遍，既啰嗦又容易把没改的字段覆盖成空。
def build_mcp(bridge: Bridge):
    from mcp.server import MCPServer

    mcp = MCPServer(
        name="mosaico-deck",
        instructions=(
            "一块桌面上的小屏设备，用来让人一眼看到 agent 在做什么，"
            "并且在需要的时候问他。"
        ),
    )

    @mcp.tool(
        description="把 agent 的当前状态显示在桌面设备上，让人不用切窗口就能看到。"
                    "只传要改的字段即可，其余保持原样。"
    )
    async def display_status(
        project: str = "",
        state: str = "",
        detail: str = "",
        target: str = "",
        progress: float = -1.0,
        files_changed: int = -1,
        tests_passed: int = -1,
        tests_total: int = -1,
        log_line: str = "",
    ) -> str:
        """更新设备上的状态页。

        state 可选：idle / thinking / running / editing / testing / waiting /
        done / error / failed。progress 是 0~1（-1 表示不改）。
        """
        kw = {
            "project": project, "state": state, "detail": detail, "target": target,
            "log_line": log_line,
        }
        if progress >= 0:
            kw["progress"] = progress
        if files_changed >= 0:
            kw["files_changed"] = files_changed
        if tests_passed >= 0:
            kw["tests_passed"] = tests_passed
        if tests_total >= 0:
            kw["tests_total"] = tests_total
        return await bridge.display_status(**kw)

    @mcp.tool(
        description="在桌面设备上向人提问，并阻塞等待回答。**这是这个设备最有用的地方** ——"
                    "人不用切窗口、不用在终端里找提示，设备会亮起来、他按一下就回答了。"
                    "返回人选中的选项原文；超时返回 'timeout'，"
                    "用户按物理键取消返回 'cancelled'（这两个要当特殊值处理，"
                    "不要当成普通选项）。"
    )
    async def ask_user(question: str, options: list[str], timeout_s: float = 120.0) -> str:
        """在设备上问人一个问题。

        options 给 2~5 个（再多人就点不准了，屏幕只有 480×480）。
        """
        return await bridge.ask_user(question, options, timeout_s)

    @mcp.tool(description="在设备上提示一条信息（不阻塞，立刻返回）。")
    async def notify(text: str, state: str = "waiting") -> str:
        return await bridge.notify(text, state)

    return mcp


async def run_mcp(bridge: Bridge) -> None:
    mcp = build_mcp(bridge)
    await bridge.start()
    await mcp.run_stdio_async()


# ---------------------------------------------------------------------------
# Demo：跑一遍假的 agent 流程，看屏幕上的真实效果
# ---------------------------------------------------------------------------
async def run_demo(bridge: Bridge) -> None:
    await bridge.start()
    bridge.state.project = "mosaico-deck"

    steps = [
        ("thinking", "正在读需求", "", None, None),
        ("editing", "定位到 refresh() 里的竞态", "src/auth/session.py", 0.15, "读了 4 个文件"),
        ("editing", "重写 token 刷新逻辑", "src/auth/session.py", 0.45, "改写完成"),
        ("editing", "补上并发用例", "tests/test_refresh.py", 0.65, "新增 2 个用例"),
        ("testing", "跑测试", "pytest tests/", 0.85, "13/14 通过"),
        ("error", "test_refresh 还是红的", "tests/test_refresh.py", None, "失败：断言超时"),
    ]
    for i, (state, detail, target, prog, logline) in enumerate(steps):
        bridge.state.state = state
        bridge.state.detail = detail
        bridge.state.target = target
        bridge.state.progress = prog
        if logline:
            bridge.state.log.append(logline)
        bridge.state.files_changed = min(i, 3)
        bridge.state.tests_passed = 13 if state == "testing" else 0
        bridge.state.tests_total = 14 if state == "testing" else 0
        n = await bridge.redraw(force_full=(i == 0))
        print(f"  步骤 {i + 1}: {state:8s} -> 发出 {n} 条 band"
              f"（{n * 28800 / 1024:.0f} KB）", file=sys.stderr)
        await asyncio.sleep(1.6)

    if bridge.auto_answer is None:
        print("\n  现在问一个问题 —— 请在设备上点一个选项（或按 AI 键取消）", file=sys.stderr)
    else:
        print(f"\n  问一个问题，并在 3 秒后自动点第 {bridge.auto_answer + 1} 个选项"
              f"（--auto-answer，用于无人验证）", file=sys.stderr)

        async def auto_tap():
            await asyncio.sleep(3.0)
            bridge.tap_button(bridge.auto_answer)

        asyncio.create_task(auto_tap())

    answer = await bridge.ask_user(
        "刚才那个断言超时，你想怎么办？",
        ["放宽超时到 5s", "查有没有真竞态", "先跳过这个用例"],
        timeout_s=20 if bridge.auto_answer is not None else 90,
    )
    print(f"  人回答: {answer}", file=sys.stderr)

    bridge.state.state = "done" if answer != "timeout" else "waiting"
    bridge.state.detail = f"你的选择：{answer}"
    bridge.state.progress = 1.0
    await bridge.redraw()
    await asyncio.sleep(4)


# ---------------------------------------------------------------------------
def run_preview(out_dir: Path) -> None:
    """不连板子，只把各种页面存成 PNG —— 调界面用。"""
    out_dir.mkdir(parents=True, exist_ok=True)
    pages = {
        "status_editing": ui.render_status(
            project="mosaico-deck", state="editing", target="src/auth/session.py",
            detail="正在重写 token 刷新逻辑", progress=0.52, elapsed_s=252,
            files_changed=7, tests_passed=12, tests_total=14,
            log=["读了 4 个文件", "定位到 refresh() 里的竞态", "改写完成，准备跑测试"],
            buttons=[("批准", "approve"), ("拒绝", "deny")])[0],
        "status_testing": ui.render_status(
            project="mosaico-deck", state="testing", target="pytest tests/",
            detail="跑了 14 个测试", progress=0.9, elapsed_s=417, files_changed=7,
            tests_passed=13, tests_total=14,
            log=["session.py 的用例过了", "test_refresh 还是红的"])[0],
        "status_done": ui.render_status(
            project="mosaico-deck", state="done", detail="14/14 全过",
            progress=1.0, elapsed_s=512, files_changed=7, tests_passed=14,
            tests_total=14)[0],
        "status_error": ui.render_status(
            project="mosaico-deck", state="error", target="tests/test_refresh.py",
            detail="断言超时：等 refresh() 返回", elapsed_s=333, files_changed=7,
            log=["重试了 2 次", "怀疑是竞态"])[0],
        "ask": ui.render_ask("刚才那个断言超时，你想怎么办？",
                             ["放宽超时到 5s", "查有没有真竞态", "先跳过这个用例"],
                             footer="点一下选择，或按 AI 键取消")[0],
        "boot": ui.render_boot(),
    }
    for name, img in pages.items():
        p = out_dir / f"{name}.png"
        img.save(p)
        print(f"  {p}")


# ---------------------------------------------------------------------------
def main() -> None:
    ap = argparse.ArgumentParser(description="mosaico-deck 的 PC 侧桥")
    ap.add_argument("--port", help="串口，默认自动找 /dev/cu.usbmodem*")
    ap.add_argument("--demo", action="store_true", help="跑一遍假的 agent 流程")
    ap.add_argument("--preview", metavar="DIR", help="只渲染页面成 PNG，不连板子")
    ap.add_argument("--auto-answer", type=int, metavar="N",
                    help="demo 模式下自动点第 N 个选项（0 起），用于无人验证")
    args = ap.parse_args()

    if args.preview:
        run_preview(Path(args.preview))
        return

    port = args.port or find_port()
    if not port:
        sys.exit("找不到板子。插上 USB，或用 --port 指定。")
    print(f"[bridge] 串口 {port}", file=sys.stderr)

    bridge = Bridge(port)
    bridge.auto_answer = args.auto_answer
    try:
        if args.demo:
            asyncio.run(run_demo(bridge))
        else:
            asyncio.run(run_mcp(bridge))
    except KeyboardInterrupt:
        pass
    finally:
        bridge.stop()


if __name__ == "__main__":
    main()
