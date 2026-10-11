// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// mosaico-deck 的编译期配置。
//
// 和 mosaico-fluidbox 的关系
// ------------------------
// 显示（band 双缓冲 + TE 同步）、触摸（含"坏报告≠松手"那个去抖）、板级版本识别
// （eFuse）这三块是从 fluidbox 直接搬过来的 —— 它们都已在真机上验证过，包括踩过的
// 那些坑。这里只留它们需要的参数，仿真相关的全部去掉。
//
// 这个工程要做什么
// --------------
// 一块**桌面的 agent 状态/交互终端**：480×480 屏 + 触摸 + 按键 + USB。
// 界面**不在板子上渲染** —— PC 侧渲染好、按 band 发过来，板子只负责显示和上报输入。
// 所以这里没有任何排版、字体、颜色的配置，那些都在 PC 上。

#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace fluidbox {
namespace config {

// ---------------------------------------------------------------------------
// 屏幕
// ---------------------------------------------------------------------------
inline constexpr int kLcdHRes = 480;
inline constexpr int kLcdVRes = 480;

// QSPI 像素时钟。
//
// **60 MHz，不是官方那种 40，也不是能跑到的最高 80。**
// 面板的刷新率（TE 实测约 58.5 Hz）才是这个系统的真实上限：喂满 60 Hz 需要约
// 54 MHz，再高纯粹是浪费。而且 80 MHz 下边沿来不及建立会偶发位错误 ——
// 表现是"某条 band 闪一下别的颜色"，而 QSPI 是只写总线，主机侧一点错都看不到。
inline constexpr int kLcdPixelClockHz = CONFIG_FBOX_DISPLAY_PCLK_MHZ * 1000 * 1000;

// QSPI 五条线的 GPIO 驱动能力。官方默认最弱挡（0，为降 EMI），但它的注释也写明
// "signal integrity fails at high clock 就往上调"。这里用 2。
inline constexpr int kLcdDriveCap = CONFIG_FBOX_LCD_DRIVE_CAP;

// 亮度百分比（0~100）。面板是 AMOLED，纯黑不发光，所以省电主要靠画面本身。
inline constexpr int kDisplayBrightness = 90;

// ---- band 划分 ------------------------------------------------------------
//
// 屏幕按行切成若干条，每条独立一块缓冲。这样：
//   · 不需要一整块 450KB 的帧缓冲（内部 SRAM 装不下）
//   · DMA 传输和 CPU 绘制可以流水线重叠
//   · **PC 侧可以只发变化的那几条 band** —— 这是这个工程省带宽的关键
inline constexpr int kBandRows = 30;
inline constexpr int kBandCount = kLcdVRes / kBandRows;
inline constexpr int kBandPixels = kLcdHRes * kBandRows;

static_assert(kBandCount * kBandRows == kLcdVRes,
              "band 行数必须整除屏幕高度，否则最后一条会漏掉几行");

// ---------------------------------------------------------------------------
// 触摸（在这个工程里只当作"按钮矩阵"用，不需要力反馈）
// ---------------------------------------------------------------------------
inline constexpr bool kTouchEnable = true;

// 手指速度的低通时间常数。
//
// **低通是必须的**：触摸坐标有量化抖动，直接微分会得到几百 px/s 的假速度。
// 这个工程只用坐标（判断点中了哪个按钮），速度其实用不到，但触摸组件统一算了 ——
// 留着以后做"滑动切换页面"。
inline constexpr float kTouchVelLpfTauS = 0.045f;

// 手指速度上限，防止一次跳点炸出荒谬数值。
inline constexpr float kTouchMaxSpeedPxS = 3500.0f;

// 触摸轮询周期（ms）。3ms 远快于 CST9220 的报告率，不会漏事件。
inline constexpr int kTouchPollPeriodMs = 3;

// ---------------------------------------------------------------------------
// USB 链路（PC ↔ 板子）
// ---------------------------------------------------------------------------
// 帧缓冲上限：一屏最多 480×480×2 = 450KB。PC 可以整屏发（约 450ms），
// 也可以只发变化的 band（几 KB，几十 ms）。这里按整屏留。
inline constexpr int kLinkFrameMaxBytes = kLcdHRes * kLcdVRes * 2;

// 一行 JSON 的长度上限（协议消息都是单行的）。
inline constexpr int kLinkLineMaxBytes = 512;

// ---------------------------------------------------------------------------
// 状态统计
// ---------------------------------------------------------------------------
inline constexpr int kStatsPeriodMs = 2000;

}  // namespace config
}  // namespace fluidbox
