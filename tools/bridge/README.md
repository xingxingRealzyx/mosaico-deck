# mosaico-deck bridge

PC 侧的桥：**把 agent 的状态渲染到桌面上那块 480×480 的屏，把人按的按钮变回结构化回答。**

对外是 **MCP server（stdio）**，所以任何支持 MCP 的 agent 都能用，不用为某一个 agent
单独适配。

## 装

```bash
cd mosaico-deck
python3 -m venv .venv
.venv/bin/pip install Pillow pyserial mcp
```

## 配到 agent 里

```json
{
  "mcpServers": {
    "mosaico-deck": {
      "command": "/Users/you/workspace/repos/mosaico-deck/.venv/bin/python",
      "args": ["/Users/you/workspace/repos/mosaico-deck/tools/bridge/bridge.py"]
    }
  }
}
```

不需要指定串口 —— 会自动找第一个 `/dev/cu.usbmodem*`。有多个设备时加 `"--port", "..."`。

## 三个工具

| 工具 | 作用 |
|---|---|
| `display_status` | 更新状态页。**所有字段都可选** —— 只传要改的，比如只追一行 `log_line` |
| `ask_user` | **在设备上问人并阻塞等待**，返回人选中的选项原文 |
| `notify` | 提示一条信息（不阻塞） |

`ask_user` 的两个特殊返回值要当特殊值处理，别当成普通选项：
`"timeout"`（没人理）和 `"cancelled"`（按了 AI 按键）。

## 试一下（不用 agent）

```bash
# 跑一遍假的 agent 流程，看屏幕上的真实效果
.venv/bin/python tools/bridge/bridge.py --demo

# 无人值守验证整条链（3 秒后自动点第 2 个选项）
.venv/bin/python tools/bridge/bridge.py --demo --auto-answer 1

# 只渲染各种页面成 PNG（不连板子，调界面用）
.venv/bin/python tools/bridge/bridge.py --preview /tmp/deck_ui
```

## 为什么界面在 PC 上渲染

板子只做三件事：显示、采集输入、通信。界面（文字、字体、布局、颜色）全在 PC 侧。

**理由不是板子性能不够，是开发效率：**

| | 板子渲染 | PC 渲染 |
|---|---|---|
| 中文 | 自己做点阵（1~2MB，难看） | 系统字体，完美且免费 |
| 抗锯齿 / emoji | 几乎没有 | 免费 |
| 改一次界面 | 重新烧录 | **存盘即生效** |

**而且带宽完全不是问题** —— 关键在 `deck.py` 里的 **band 差分**：整屏按 16 条
30 行的 band 切割，**只发变化的那几条**。实测：

```
首次整屏           -> 16 条 band (450 KB)  131.0 ms
内容完全没变        ->  0 条 band (  0 KB)    0.3 ms
只加一行日志        ->  1 条 band ( 28 KB)    8.7 ms
只改进度            ->  2 条 band ( 56 KB)   16.6 ms
```

**8.7ms 落在一次面板刷新（17ms）之内，所以不会撕裂。** 这一点很要紧：整屏 450KB
在 60MHz QSPI 上要 15.4ms，几乎等于一次刷新周期，稍微一抖就会跨越刷新、画面撕裂
（实测一根横扫的亮条会"弯"）。只发变化的部分就彻底避开了。

## 点击是怎么变成答案的

**布局是 PC 侧定的，所以点击的含义也只有 PC 知道。** 板子只上报"手指落在 (x,y)"，
由 `ui.py` 里记下的按钮矩形做命中判定，热区上下左右各放宽 6px（手指比鼠标粗）。

这样板子保持成一个纯哑终端 —— 加一个按钮、改一次布局，都不用重新烧录固件。
