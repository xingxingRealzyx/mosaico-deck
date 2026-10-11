// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// input —— "重置流体"按键
//
// 为什么不是原项目的按钮驱动
// --------------------------
// V4C38/esp32-fluidbox 的 button.c 是从 TCA9554 IO 扩展器上读 PWR 键的；
// ESP-Mosaico 上没有那颗芯片，板载可用的只有接到 GPIO7 的 AI 按键（低电平
// 有效）。所以这里保留原驱动"短按/长按"的判定行为，只把底层从 I2C 扩展器换成
// 普通 GPIO + 内部上拉。
//
// 为什么轮询而不是中断
// --------------------
// 按键接在 GPIO7 上，本身就是机械触点，抖动靠软件窗口滤掉就够了；主循环本来
// 就有约 25 ms 的节拍，多一个中断只会多一条并发路径。所以 GPIO_INTR_DISABLE，
// 由调用者周期性调用 Poll()。

#pragma once

#include "esp_err.h"

namespace input {

class ResetButton {
 public:
    ResetButton() = default;
    ~ResetButton() = default;

    ResetButton(const ResetButton&) = delete;
    ResetButton& operator=(const ResetButton&) = delete;

    // 配置 GPIO，打开内部上拉。
    esp_err_t Init();

    // 一次采样能报出的事件。
    enum class Event : uint8_t {
        kNone,
        kShortPress,  // 按下后 1.5 秒内松开
        kLongPress,   // 按住超过 1.5 秒 —— **在按住期间就报**，不用等松手
    };

    // 周期性调用（约每 25 ms 一次）。
    //
    // **必须只调这一个，不要拆成两个函数。**
    // 这里踩过一次：原来有 TakeShortPress() 和 TakeLongPress() 两个函数，各自
    // 维护同一份去抖状态（stable_ / raw_pressed_）。每轮依次调用时，第二个看到的
    // 状态已经被第一个改过了 —— 去抖状态机直接乱掉，长按和短按**双双失灵**。
    // 症状就是用户说的"没反应"。
    //
    // 长按在按住期间就报（不等松手）：这一档是用来"边按边试"的（长按切换触摸
    // 力度，手一松就能立刻去摸屏幕感受新力度），等松手才反馈的话人已经离开屏幕了。
    Event Poll();

 private:
    bool raw_pressed_ = false;   // 去抖后的电平
    bool stable_ = false;
    int64_t press_start_us_ = 0;
    bool reported_ = false;
    bool long_reported_ = false;  // 这一次按住是否已经报过长按
};

}  // namespace input
