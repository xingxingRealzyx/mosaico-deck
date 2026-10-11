// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// touch.cpp -- CST9220 capacitive touch

#include "touch.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst9220.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fbox/board.h"
#include "fbox/config.h"

namespace fluidbox {
namespace touch {
namespace {

constexpr const char* kTag = "touch";

// Coordinate maximum in the touch reports. Mosaico is 480x480, and the official
// code passes it the same way (esp_lcd_touch's mirror is (max - value), so what
// must be passed here is the **maximum valid coordinate**).
constexpr uint16_t kTouchXMax = config::kLcdHRes - 1;
constexpr uint16_t kTouchYMax = config::kLcdVRes - 1;

// The official BSP uses a 100 ms I2C timeout. **It must be non-zero** -- the
// underlying comment says it plainly: "Left at 0 the transfer waits forever".
// And this read happens inside the render/simulation cycle, so once the bus is
// wedged it would freeze the whole picture.
constexpr uint32_t kTouchI2cTimeoutMs = 100;

// **How long without a valid point decoded before it really counts as a release
// (milliseconds).**
//
// This is the fix for "touch flickering madly", and the cause was caught from the
// logs:
//
//     W cst9220: Malformed report header: first=0xab marker=0xab
//
// **Reports read back over I2C are occasionally corrupt** (this bus is shared
// with the 100 Hz IMU). A corrupt report yields no point, and "no point decoded"
// was being treated as "release" -- so while the finger was held still, the log
// spammed press-release pairs, repeating the same coordinate dozens of times.
// **The force switched on and off every few milliseconds**, which amounts to
// constantly exciting the system.
//
// The first version debounced on "4 consecutive empty reports" (12 ms) and that
// was **not enough** -- measured flicker gaps were tens of milliseconds.
// Changed to a time-based rule: if a valid point was decoded at least once within
// 100 ms, the finger is considered still down.
//
// The key point is to get the semantics right: **a failed read / corrupt report
// = "unknown", and must not be treated as "release".**
constexpr int64_t kReleaseTimeoutUs = 100 * 1000;

// All writes to state_ below happen inside this critical section, paired with
// Get().
float ClampSpeed(float v) {
    if (v > config::kTouchMaxSpeedPxS) {
        return config::kTouchMaxSpeedPxS;
    }
    if (v < -config::kTouchMaxSpeedPxS) {
        return -config::kTouchMaxSpeedPxS;
    }
    return v;
}

}  // namespace



// Release decision: only counts as a release once kReleaseTimeoutUs has passed
// since the last valid point was decoded.
void Touchscreen::MaybeRelease(int64_t now_us) {
    if (!state_.active || last_valid_us_ == 0) {
        return;
    }
    if (now_us - last_valid_us_ < kReleaseTimeoutUs) {
        return;  // still inside the tolerance window, treat the finger as down
    }
    portENTER_CRITICAL(&lock_);
    state_.active = false;
    state_.vx = 0.0f;
    state_.vy = 0.0f;
    portEXIT_CRITICAL(&lock_);
}

esp_err_t Touchscreen::Init(i2c_master_bus_handle_t bus) {
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config::kTouchEnable) {
        ESP_LOGI(kTag, "touch disabled by configuration");
        return ESP_OK;
    }

    // INT is watched here: configure it as a plain input and pass GPIO_NUM_NC to
    // the driver. Two pieces of logic managing the same pin at once would fight
    // each other (the driver would enable a GPIO interrupt by default).
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << fluidbox::board::kPinTouchInt,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // INT is active low; pull-up keeps it high when released
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t int_err = gpio_config(&int_cfg);
    if (int_err != ESP_OK) {
        ESP_LOGW(kTag, "failed to configure INT pin GPIO%d: %s",
                 fluidbox::board::kPinTouchInt, esp_err_to_name(int_err));
    }

    // Field-by-field assignment: initializing aggregates with macros in C++ runs
    // into -Wmissing-field-initializers and designated-initializer ordering
    // pitfalls (this project already hit them on the display side).
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST9220_ADDRESS;
    io_config.scl_speed_hz = fluidbox::board::kI2cFreqHz;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.disable_control_phase = 1;
    io_config.transaction_timeout_ms = kTouchI2cTimeoutMs;

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to create touch IO: %s", esp_err_to_name(err));
        return err;
    }

    // Orientation at rotation 0: all three of the official BSP's
    // BSP_TOUCH_BASE_* values are false.
    const esp_lcd_touch_config_t touch_config = {
        .x_max = kTouchXMax,
        .y_max = kTouchYMax,
        .rst_gpio_num = GPIO_NUM_NC,  // official BSP_LCD_TOUCH_RST = GPIO_NUM_NC, not wired
        .int_gpio_num = GPIO_NUM_NC,  // watched here instead, see above
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };

    esp_lcd_touch_handle_t tp = nullptr;
    err = esp_lcd_touch_new_i2c_cst9220(io, &touch_config, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "CST9220 init failed: %s (touch unavailable, other features unaffected)",
                 esp_err_to_name(err));
        esp_lcd_panel_io_del(io);
        return err;
    }

    io_ = io;
    tp_ = tp;
    ESP_LOGI(kTag, "CST9220 ready (INT=GPIO%d, shared I2C SDA=%d SCL=%d)",
             fluidbox::board::kPinTouchInt, fluidbox::board::Active().i2c_sda,
             fluidbox::board::Active().i2c_scl);
    return ESP_OK;
}

State Touchscreen::Get() {
    portENTER_CRITICAL(&lock_);
    const State copy = state_;
    portEXIT_CRITICAL(&lock_);
    return copy;
}

void Touchscreen::Poll() {
    if (tp_ == nullptr) {
        return;
    }

    // INT is active low and is only pulled low when there is a touch event
    // (press/move/release).
    //
    // **Check this pin first before deciding whether to read I2C**: the underlying
    // read_data reads the bus unconditionally, and this bus also carries the IMU
    // (100 Hz). Without gating, a poll with nobody touching would still pay for
    // one I2C transfer and compete with the IMU for the bus.
    if (gpio_get_level(static_cast<gpio_num_t>(fluidbox::board::kPinTouchInt)) != 0) {
        // No new event -- the finger may genuinely have left (INT is only pulled
        // low while there are events), or this may just be a quiet gap. Go by
        // "the last valid point decoded", see kReleaseTimeoutUs.
        MaybeRelease(esp_timer_get_time());
        return;
    }

    esp_lcd_touch_handle_t tp = static_cast<esp_lcd_touch_handle_t>(tp_);
    if (esp_lcd_touch_read_data(tp) != ESP_OK) {
        return;
    }
    reads_ = reads_ + 1;

    const int64_t now_us = esp_timer_get_time();
    esp_lcd_touch_point_data_t points[1] = {};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(tp, points, &count, 1) != ESP_OK || count == 0) {
        // Release event: the CST9220 sends a record with the status bit cleared
        // while the coordinates are still the old ones, so this must be decided by
        // count, not by the coordinates.
        //
        // **No point decoded != release.** The report may be corrupt (the log
        // catches "Malformed report header"), or it may be an empty report per
        // protocol. Only when the "last valid point" is older than
        // kReleaseTimeoutUs does it really count as a release -- otherwise the
        // force would switch on and off every few milliseconds.
        MaybeRelease(now_us);
        return;
    }
    last_valid_us_ = now_us;

    const float x = static_cast<float>(points[0].x);
    const float y = static_cast<float>(points[0].y);

    // The whole tail end is inside **one** critical section: position, velocity
    // and active must be seen by readers together, otherwise Get() could get a
    // combination such as "new position + old velocity".
    portENTER_CRITICAL(&lock_);
    if (state_.active && last_us_ > 0) {
        const float dt = static_cast<float>(now_us - last_us_) * 1e-6f;
        if (dt > 1e-4f) {
            // Differentiate for velocity, then apply a first-order low-pass.
            //
            // **The low-pass is necessary**: the touch coordinates have
            // quantisation jitter, and differentiating directly would produce
            // fake velocities of hundreds or even thousands of px/s, making the
            // fluid jitter on its own. A time constant of tens of milliseconds
            // keeps up with the finger while suppressing the jitter.
            const float nvx = ClampSpeed((x - static_cast<float>(state_.x)) / dt);
            const float nvy = ClampSpeed((y - static_cast<float>(state_.y)) / dt);
            const float a = dt / (config::kTouchVelLpfTauS + dt);
            state_.vx += (nvx - state_.vx) * a;
            state_.vy += (nvy - state_.vy) * a;
        }
    } else {
        // Just pressed: there is no history, so velocity starts at 0.
        state_.vx = 0.0f;
        state_.vy = 0.0f;
    }
    state_.x = static_cast<int>(x);
    state_.y = static_cast<int>(y);
    state_.active = true;
    portEXIT_CRITICAL(&lock_);
    last_us_ = now_us;
}

}  // namespace touch
}  // namespace fluidbox
