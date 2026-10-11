// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// input 实现。见 include/input.h 里的原理说明。
//
// 两个 bool 的分工
// ----------------
//   raw_pressed_ —— 已经确认（去抖后）的电平，业务逻辑只看它
//   stable_      —— 上一次采样值，只用于和本次采样比较
// 每次调用先和 stable_ 比：不同就只更新 stable_ 并返回，等下一次采样再来确认。
// 调用周期约 25 ms，所以"连续两次相同"正好是 50 ms 的消抖窗。

#include "input.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fbox/board.h"

namespace input {
namespace {

constexpr const char* kTag = "input";

// 板级引脚由 config 组件给出，不写死数字；取个别名免得每处都写全限定名。
constexpr int kButtonPin = fluidbox::board::kPinButtonAi;

// 短按上限：按下到松开不超过 1.5 秒。更长的按下算长按，不报告（Mosaico 上
// 没有"长按关机"那套硬件联动，但行为和原项目保持一致）。
// **长按门限：按住超过它算长按，松开在它之内算短按。**
//
// 原来是 1.5 秒 —— 太长了。实测用户的自然按法是 160~250ms 的轻点（日志里
// 六次短按分别是 159/203/250/201/190/160 ms），根本够不到 1.5 秒，
// 于是长按（切换触摸力度）永远触发不了。
//
// 0.8 秒仍然明显区别于"轻点"，但按住不放的意图能表达出来。
constexpr int64_t kShortPressMaxUs = 800'000;

}  // namespace

esp_err_t ResetButton::Init() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << kButtonPin;
    cfg.mode = GPIO_MODE_INPUT;
    // 按键另一端接 GND、低电平有效，所以必须靠内部上拉给出空闲时的高电平。
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "AI 按键 (GPIO%d) 配置失败: %s", kButtonPin,
                 esp_err_to_name(err));
        return err;
    }

    // 从上电时的"未按下"状态重新开始，免得 Init 之前残留的采样值造成误报。
    raw_pressed_ = false;
    stable_ = false;
    press_start_us_ = 0;
    reported_ = false;

    ESP_LOGI(kTag, "AI 按键就绪 (GPIO%d, 低电平有效)", kButtonPin);
    return ESP_OK;
    // 诊断用：把官方的 BOOT 键脚也配成输入（只读，上拉）。
    const gpio_config_t probe_cfg = {
        .pin_bit_mask = 1ULL << 61,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&probe_cfg);

}

ResetButton::Event ResetButton::Poll() {
    // 低电平有效：读到 0 才是按下。
    const bool pressed = (gpio_get_level(static_cast<gpio_num_t>(kButtonPin)) == 0);
    const int64_t now = esp_timer_get_time();

    // 去抖第一步：和上次采样不同，说明还在抖动，先记下来等下一次确认。
    if (pressed != stable_) {
        stable_ = pressed;
        return Event::kNone;
    }

    // 电平已确认。如果是"刚按下"，只需要起表。
    if (pressed != raw_pressed_) {
        raw_pressed_ = pressed;
        if (pressed) {
            press_start_us_ = now;
            reported_ = false;
            long_reported_ = false;
            return Event::kNone;
        }
        // 松开沿：短按在这里判定。
        if (reported_) {
            return Event::kNone;
        }
        reported_ = true;
        const int64_t held_us = now - press_start_us_;
        if (held_us >= kShortPressMaxUs) {
            // 长按已经在按住期间报过了，这里不再重复。
            return Event::kNone;
        }
        ESP_LOGI(kTag, "短按 %lld ms", static_cast<long long>(held_us / 1000));
        return Event::kShortPress;
    }

    // 状态没变。如果正按着且已经超过长按门限，就地报一次。
    if (pressed && !long_reported_ && now - press_start_us_ >= kShortPressMaxUs) {
        long_reported_ = true;
        ESP_LOGI(kTag, "长按 -> 切换触摸力度");
        return Event::kLongPress;
    }
    return Event::kNone;
}

}  // namespace input
