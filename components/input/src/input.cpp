// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// input implementation. See the rationale in include/input.h.
//
// Division of labour between the two bools
// ----------------------------------------
//   raw_pressed_ -- the confirmed (debounced) level; business logic only looks at this
//   stable_      -- the previous sample value, used only to compare against this sample
// Each call first compares with stable_: if they differ, only update stable_ and
// return, then wait for the next sample to confirm. The call period is about
// 25 ms, so "two identical samples in a row" is exactly a 50 ms debounce window.

#include "input.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fbox/board.h"

namespace input {
namespace {

constexpr const char* kTag = "input";

// The board pin comes from the config component rather than a hard-coded number;
// take an alias so every use does not need the fully qualified name.
constexpr int kButtonPin = fluidbox::board::kPinButtonAi;

// Short-press upper bound: no more than 1.5 seconds from press to release. A
// longer hold counts as a long press and is not reported (the Mosaico has no
// "long press to power off" hardware linkage, but the behaviour stays consistent
// with the original project).
// **Long-press threshold: holding beyond it counts as a long press, releasing
// within it counts as a short press.**
//
// It used to be 1.5 seconds -- far too long. Measured natural user presses are
// 160~250 ms taps (six short presses in the log were 159/203/250/201/190/160 ms
// respectively), nowhere near 1.5 seconds, so the long press (toggle touch force)
// could never trigger.
//
// 0.8 seconds is still clearly distinct from a "tap", yet the intent of holding
// the button down can be expressed.
constexpr int64_t kShortPressMaxUs = 800'000;

}  // namespace

esp_err_t ResetButton::Init() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << kButtonPin;
    cfg.mode = GPIO_MODE_INPUT;
    // The other side of the button goes to GND and it is active low, so the
    // internal pull-up is required to provide the high level when idle.
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to configure AI button (GPIO%d): %s", kButtonPin,
                 esp_err_to_name(err));
        return err;
    }

    // Restart from the "not pressed" state as of power-up, so that sample values
    // left over before Init cannot cause a spurious report.
    raw_pressed_ = false;
    stable_ = false;
    press_start_us_ = 0;
    reported_ = false;

    ESP_LOGI(kTag, "AI button ready (GPIO%d, active low)", kButtonPin);
    return ESP_OK;
    // Diagnostic: also configure the official BOOT key pin as an input
    // (read-only, pull-up).
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
    // Active low: it counts as pressed only when 0 is read.
    const bool pressed = (gpio_get_level(static_cast<gpio_num_t>(kButtonPin)) == 0);
    const int64_t now = esp_timer_get_time();

    // Debounce step one: differing from the previous sample means it is still
    // bouncing; note it down and wait for the next confirmation.
    if (pressed != stable_) {
        stable_ = pressed;
        return Event::kNone;
    }

    // The level is confirmed. If this is a "just pressed", only start the clock.
    if (pressed != raw_pressed_) {
        raw_pressed_ = pressed;
        if (pressed) {
            press_start_us_ = now;
            reported_ = false;
            long_reported_ = false;
            return Event::kNone;
        }
        // Release edge: the short press is decided here.
        if (reported_) {
            return Event::kNone;
        }
        reported_ = true;
        const int64_t held_us = now - press_start_us_;
        if (held_us >= kShortPressMaxUs) {
            // The long press was already reported while held; do not repeat it here.
            return Event::kNone;
        }
        ESP_LOGI(kTag, "short press %lld ms", static_cast<long long>(held_us / 1000));
        return Event::kShortPress;
    }

    // State unchanged. If it is being held and the long-press threshold has
    // already passed, report it right here, once.
    if (pressed && !long_reported_ && now - press_start_us_ >= kShortPressMaxUs) {
        long_reported_ = true;
        ESP_LOGI(kTag, "long press -> toggle touch force");
        return Event::kLongPress;
    }
    return Event::kNone;
}

}  // namespace input
