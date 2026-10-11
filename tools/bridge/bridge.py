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
        self.deck = Deck(port, on_event=self._on_deck_event, on_audio=self._on_deck_audio)
        self.state = DeckState()
        self.buttons: list[ui.Button] = []
        self.pending: asyncio.Future | None = None
        self.loop: asyncio.AbstractEventLoop | None = None
        self._lock = threading.Lock()
        self._dirty = False
        self.ask_options: list[str] = []
        self.auto_answer: int | None = None

        # 录音：板子在 AI 按键按住期间把 PCM 推上来，这里攒起来。
        self._rec_lock = threading.Lock()
        self._rec_pcm = bytearray()
        self._recording = False

        # 状态页上的"批准/拒绝"用这个 future 接。
        self.approval: asyncio.Future | None = None

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

    def _on_deck_audio(self, pcm: bytes) -> None:
        """板子推上来的录音数据。**在接收线程里，只做攒数据。**"""
        with self._rec_lock:
            self._rec_pcm.extend(pcm)

    def _handle_event(self, ev: dict) -> None:
        kind = ev.get("t")
        if kind == "touch":
            if ev.get("ev") == "down":
                self._on_tap(int(ev.get("x", 0)), int(ev.get("y", 0)))
        elif kind == "key":
            ev_name = ev.get("ev", "")
            if ev_name == "voice_end":
                # 松开 AI 按键 = 说完了。交给录音的等待方。
                print(f"[bridge] 录音结束（收到 {len(self._rec_pcm)} 字节）", file=sys.stderr)
            elif self.pending is not None and not self.pending.done():
                # 其他按键 = 取消当前问询。物理按键比在屏幕上找"取消"更顺手。
                self.pending.set_result(None)
        elif kind == "ack":
            pass

    def _on_tap(self, x: int, y: int) -> None:
        # **没有待回答的问询时，忽略点击。**
        #
        # 实测会多收到一次点击：手指点下去触发一次，而 ask_user 返回后立刻重画了
        # 状态页（self.buttons 被换成状态页的按钮），紧接着松手/触摸抖动又上报
        # 一次，落在同一个坐标上却已经没有对应按钮了 —— 日志里表现为
        # "点中按钮 0" 紧跟一条 "点在空白处 (同样的坐标)"。
        # 无害，但会让日志难读，而且以后给状态页接上"批准/拒绝"之后，这多出来
        # 的一下可能会误触。
        for b in self.buttons:
            if not b.hit(x, y):
                continue
            print(f"[bridge] 点中按钮 {b.value} ({x},{y})", file=sys.stderr)
            # 问询页的按钮和状态页的"批准/拒绝"共用一个按钮列表，
            # 谁在等就交给谁。
            if self.pending is not None and not self.pending.done():
                self.pending.set_result(b.value)
            elif self.approval is not None and not self.approval.done():
                self.approval.set_result(b.value)
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
                # 有人在等批准时才画按钮 —— 平时画着是噪音，而且误触代价大。
                buttons=([("批准", "approve"), ("拒绝", "deny")]
                         if self.approval is not None and not self.approval.done()
                         else None),
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

    async def speak(self, text: str, voice: str, rate: int) -> str:
        """让设备说话。

        用 macOS 的 `say` 合成、`ffmpeg` 转成板子要的格式，再按播放速率推下去。

        **为什么 TTS 放在 PC 上**：板子只有 512KB SRAM、没有神经网络加速，
        跑不了任何像样的 TTS。而 PC 上的 `say` 是系统自带的、音质好、还支持中文。
        板子只负责把 PCM 播出来 —— 这也是整个架构的思路。
        """
        import subprocess
        import tempfile

        if not text.strip():
            return "文本为空"

        with tempfile.TemporaryDirectory() as td:
            aiff = f"{td}/t.aiff"
            pcm = f"{td}/t.pcm"
            cmd = ["say", "-o", aiff]
            if voice:
                cmd += ["-v", voice]
            if rate > 0:
                cmd += ["-r", str(rate)]
            cmd.append(text)
            try:
                subprocess.run(cmd, check=True, capture_output=True, timeout=60)
                subprocess.run(
                    ["ffmpeg", "-y", "-loglevel", "error", "-i", aiff,
                     "-f", "s16le", "-acodec", "pcm_s16le",
                     "-ar", "48000", "-ac", "2", pcm],
                    check=True, capture_output=True, timeout=60)
            except FileNotFoundError as e:
                return f"缺少命令: {e.filename}（macOS 自带 say；ffmpeg 需要 brew install）"
            except subprocess.CalledProcessError as e:
                return f"合成失败: {e}"
            except subprocess.TimeoutExpired:
                return "合成超时"

            data = Path(pcm).read_bytes()

        secs = len(data) / 48000 / 4
        await asyncio.to_thread(self.deck.speak_pcm, data)
        return f"说完了（{secs:.1f} 秒）"

    async def listen(self, timeout_s: float) -> tuple[str, str]:
        """等用户按住 AI 按键说一段话，把 WAV 存下来并尝试转成文字。

        返回 (文本, 说明)。**STT 是可配置的** —— 本地没有装识别引擎，
        所以默认只存 WAV 并告诉你路径；配了 DECK_STT_CMD 就自动转文字。
        """
        import os
        import subprocess
        import tempfile
        import wave

        with self._rec_lock:
            self._rec_pcm.clear()

        await self.display_status(state="waiting", detail="按住 AI 按键说话",
                                  target="松手结束", log_line="在等你的语音")
        print("[bridge] 等用户按住 AI 按键…", file=sys.stderr)

        # 等录音开始（板子收到按键后开始推数据），再等它结束。
        t0 = time.time()
        started = False
        while time.time() - t0 < timeout_s:
            if not started and len(self._rec_pcm) > 0:
                started = True
                print("[bridge] 检测到录音开始", file=sys.stderr)
            if started and len(self._rec_pcm) > 0:
                # 连续 700ms 没有新数据就认为说完了
                n = len(self._rec_pcm)
                await asyncio.sleep(0.7)
                if len(self._rec_pcm) == n:
                    break
            await asyncio.sleep(0.05)

        with self._rec_lock:
            data = bytes(self._rec_pcm)

        if not data:
            return "", "没听到任何声音（按住 AI 按键时才会录音）"

        out_dir = Path(tempfile.gettempdir()) / "mosaico-deck"
        out_dir.mkdir(exist_ok=True)
        wav_path = out_dir / f"voice_{int(time.time())}.wav"
        with wave.open(str(wav_path), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(48000)
            w.writeframes(data)

        secs = len(data) / 48000 / 4
        stt_cmd = os.environ.get("DECK_STT_CMD", "")
        if not stt_cmd:
            return "", (f"录到 {secs:.1f} 秒，已存到 {wav_path}。"
                        f"要自动转文字，设环境变量 DECK_STT_CMD 为一条"
                        f"「接收 wav 路径、往 stdout 打文字」的命令。")

        try:
            # 用 shell 是为了让用户能写带参数的命令（例如 `whisper --model small`）
            r = subprocess.run(f'{stt_cmd} "{wav_path}"', shell=True,
                               capture_output=True, text=True, timeout=300)
            text = r.stdout.strip()
            if not text:
                return "", f"识别命令没有输出（stderr: {r.stderr.strip()[:200]}）"
            return text, f"识别自 {secs:.1f} 秒语音（{wav_path}）"
        except subprocess.TimeoutExpired:
            return "", "识别超时"
        except Exception as e:  # noqa: BLE001
            return "", f"识别失败: {e}"

    async def request_approval(self, summary: str, detail: str) -> str:
        """显示一个待批准的改动，等用户在设备上按「批准」或「拒绝」。

        和 ask_user 的区别：这个是状态页 + 两个固定按钮，适合"我要动一个危险操作，
        你确认一下"这种场景；ask_user 是问一个问题、给若干选项。
        """
        loop = asyncio.get_running_loop()
        self.approval = loop.create_future()
        self.state.state = "waiting"
        self.state.detail = summary
        self.state.target = detail
        self.state.log.append("等你批准")
        await self.redraw(force_full=True)

        try:
            value = await asyncio.wait_for(self.approval, timeout=300)
        except asyncio.TimeoutError:
            value = "timeout"
        finally:
            self.approval = None
            self.state.state = "running" if value == "approve" else "idle"
            self.state.detail = f"你选择了：{value}"
            await self.redraw(force_full=True)
        return value

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

    @mcp.tool(
        description="**让设备开口说话。** 用当前机器的语音合成，通过 USB 送给设备播出来。"
                    "适合「活干完了」「需要你看一眼」这类不需要打断但希望被知道的时刻 ——"
                    "比弹通知好，因为它就在你桌上，而且不用你抬手。"
    )
    async def speak(text: str, voice: str = "", rate: int = 0) -> str:
        """让设备把 text 念出来。

        voice 是 macOS 的音色名（`say -v '?'` 可以列，中文常用 Tingting）。
        空字符串表示用系统默认。rate 是语速（词/分），0 表示默认。
        """
        return await bridge.speak(text, voice, rate)

    @mcp.tool(
        description="**听用户说一段话。** 调用后设备会提示「按住 AI 按键说话」，"
                    "用户按住说话、松手结束，然后把语音转成文字返回。"
                    "适合需要用户给出较长、开放的回答时 —— 比在 480×480 上选按钮自然得多。"
    )
    async def listen(timeout_s: float = 60.0) -> str:
        """等用户按住 AI 按键说一段话，返回识别出的文字。

        如果这台机器没配语音识别（环境变量 DECK_STT_CMD），会返回一条说明，
        里面带着录音 WAV 的路径。
        """
        text, note = await bridge.listen(timeout_s)
        if text:
            return f"{text}\n（{note}）"
        return note

    @mcp.tool(
        description="在设备上请求人工批准一个危险或不可逆的操作，阻塞等待用户按"
                    "「批准」或「拒绝」。返回 'approve' / 'deny' / 'timeout'。"
    )
    async def request_approval(summary: str, detail: str = "") -> str:
        """显示一个待批准的操作，等用户按设备上的按钮。"""
        return await bridge.request_approval(summary, detail)

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
