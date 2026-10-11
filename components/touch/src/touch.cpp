// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// touch.cpp —— CST9220 电容触摸

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

// 触摸报告里的坐标上限。Mosaico 是 480x480，官方也是这么传的
//（esp_lcd_touch 的 mirror 是 (max - value)，所以这里要传**最大有效坐标**）。
constexpr uint16_t kTouchXMax = config::kLcdHRes - 1;
constexpr uint16_t kTouchYMax = config::kLcdVRes - 1;

// 官方 BSP 的 I2C 超时是 100ms。**必须是非 0** —— 底层注释写得很清楚：
// "Left at 0 the transfer waits forever"。而这里的读发生在渲染/仿真的周期里，
// 一旦总线被拉死就会把整个画面卡住。
constexpr uint32_t kTouchI2cTimeoutMs = 100;

// **多久没解出有效的点，才算真的松手（毫秒）。**
//
// 这是"触摸疯狂闪断"的解法，而且原因是从日志里抓到的：
//
//     W cst9220: Malformed report header: first=0xab marker=0xab
//
// **I2C 读回来的报告时不时是坏的**（这条总线和 100Hz 的 IMU 共用）。坏报告解不出
// 点，而"解不出点"被当成了"松手" —— 于是按住不动时日志里"按下-抬起"成对刷屏，
// 同一个坐标重复几十次。**受力每几毫秒通断一次**，等于一直在激励系统。
//
// 第一版按"连续 4 次空报告"（12ms）去抖，**不够** —— 实测闪断的间隔有几十毫秒。
// 改成基于时间：100ms 内只要解出过一次有效的点，就认为手指还在。
//
// 关键是语义要对：**读失败/报告坏 = "不知道"，不能当成"松手"。**
constexpr int64_t kReleaseTimeoutUs = 100 * 1000;

// 下面所有对 state_ 的写入都在这个临界区里，和 Get() 配对。
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



// 松手判定：距最后一次解出有效点超过 kReleaseTimeoutUs 才算松手。
void Touchscreen::MaybeRelease(int64_t now_us) {
    if (!state_.active || last_valid_us_ == 0) {
        return;
    }
    if (now_us - last_valid_us_ < kReleaseTimeoutUs) {
        return;  // 还在容错窗内，认为手指仍然按着
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
        ESP_LOGI(kTag, "触摸已按配置关闭");
        return ESP_OK;
    }

    // INT 自己看：配成普通输入即可，驱动那边传 GPIO_NUM_NC。
    // 两套逻辑同时管同一个脚会互相打架（驱动默认会开 GPIO 中断）。
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << fluidbox::board::kPinTouchInt,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // INT 低有效，上拉保证松手时是高
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t int_err = gpio_config(&int_cfg);
    if (int_err != ESP_OK) {
        ESP_LOGW(kTag, "INT 脚 GPIO%d 配置失败: %s",
                 fluidbox::board::kPinTouchInt, esp_err_to_name(int_err));
    }

    // 逐字段赋值：C++ 里用宏初始化聚合体会触到 -Wmissing-field-initializers 和
    // 指示符顺序的坑（本工程在显示那边踩过）。
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
        ESP_LOGE(kTag, "创建触摸 IO 失败: %s", esp_err_to_name(err));
        return err;
    }

    // rotation 0 下的方向：官方 BSP 的 BSP_TOUCH_BASE_* 三个都是 false。
    const esp_lcd_touch_config_t touch_config = {
        .x_max = kTouchXMax,
        .y_max = kTouchYMax,
        .rst_gpio_num = GPIO_NUM_NC,  // 官方 BSP_LCD_TOUCH_RST = GPIO_NUM_NC，没接
        .int_gpio_num = GPIO_NUM_NC,  // 自己看，见上
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };

    esp_lcd_touch_handle_t tp = nullptr;
    err = esp_lcd_touch_new_i2c_cst9220(io, &touch_config, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "CST9220 初始化失败: %s（触摸不可用，其余功能不受影响）",
                 esp_err_to_name(err));
        esp_lcd_panel_io_del(io);
        return err;
    }

    io_ = io;
    tp_ = tp;
    ESP_LOGI(kTag, "CST9220 就绪 (INT=GPIO%d, 复用 I2C SDA=%d SCL=%d)",
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

    // INT 低有效，且只在有触摸事件（按下/移动/抬起）时才拉低。
    //
    // **先看这个脚再决定要不要读 I2C**：底层 read_data 会无条件读一次总线，
    // 而这条总线还挂着 IMU（100Hz）。不做门控的话，一次没人碰的轮询也要付
    // 一次 I2C 传输的代价，而且会和 IMU 抢总线。
    if (gpio_get_level(static_cast<gpio_num_t>(fluidbox::board::kPinTouchInt)) != 0) {
        // 没有新事件 —— 手指可能真的离开了（INT 只在有事件时拉低），
        // 也可能只是这段空窗。以"最后一次解出有效点"为准，见 kReleaseTimeoutUs。
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
        // 抬起事件：CST9220 会发一条状态位被清掉的记录，坐标仍然是旧的，
        // 所以必须靠 count 判断，不能靠坐标。
        //
        // **读不出点 ≠ 松手。** 报告可能是坏的（日志里能抓到 Malformed report
        // header），也可能是协议上的空报告。只有当"最后一次有效点"超过
        // kReleaseTimeoutUs 才算真的松手 —— 否则受力会每几毫秒通断一次。
        MaybeRelease(now_us);
        return;
    }
    last_valid_us_ = now_us;

    const float x = static_cast<float>(points[0].x);
    const float y = static_cast<float>(points[0].y);

    // 整段收尾在**一个**临界区里：位置、速度、active 必须一起被读者看到，
    // 否则 Get() 可能拿到"新位置 + 旧速度"这种组合。
    portENTER_CRITICAL(&lock_);
    if (state_.active && last_us_ > 0) {
        const float dt = static_cast<float>(now_us - last_us_) * 1e-6f;
        if (dt > 1e-4f) {
            // 差分求速度，再做一阶低通。
            //
            // **低通是必须的**：触摸的坐标有量化抖动，直接微分会得到几百甚至上千
            // px/s 的假速度，液体就会自己乱抖。时间常数取几十毫秒，跟得上手指、
            // 又压得住抖动。
            const float nvx = ClampSpeed((x - static_cast<float>(state_.x)) / dt);
            const float nvy = ClampSpeed((y - static_cast<float>(state_.y)) / dt);
            const float a = dt / (config::kTouchVelLpfTauS + dt);
            state_.vx += (nvx - state_.vx) * a;
            state_.vy += (nvy - state_.vy) * a;
        }
    } else {
        // 刚按下：没有历史，速度从 0 开始。
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
