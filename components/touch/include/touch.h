// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// touch.h —— CST9220 电容触摸
//
// 用途：让用户能用手指对粒子施力。
//
// 设计要点
// --------
// 1. **INT 门控**。底层 `esp_lcd_touch_read_data()` 会无条件读一次 I2C，而这条
//    总线是和 IMU 共用的（IMU 100Hz）。所以这里自己看 INT 脚，只有在真的有触摸
//    事件时才去读总线。
// 2. **驱动不碰 INT**。`int_gpio_num` 传 `GPIO_NUM_NC`，避免和上面那套自己看的
//    逻辑打架（驱动默认会配 GPIO 中断）。
// 3. **坐标不做翻转**。官方 BSP 在 rotation 0 下的基准就是
//    `swap_xy=false, mirror_x=false, mirror_y=false`，而本工程固定用 rotation 0。

#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace fluidbox {
namespace touch {

// 手指状态。坐标是**面板像素**，和仿真/渲染的屏幕坐标是同一套。
struct State {
    bool active = false;
    int x = 0;
    int y = 0;
    float vx = 0.0f;  // 像素/秒，已低通
    float vy = 0.0f;
};

class Touchscreen {
 public:
    Touchscreen() = default;
    Touchscreen(const Touchscreen&) = delete;
    Touchscreen& operator=(const Touchscreen&) = delete;

    // 在给定的 I2C 总线上创建触摸设备。总线必须是已经建好的那条
    //（Mosaico 上和 IMU 共用 GPIO56/3）。
    esp_err_t Init(i2c_master_bus_handle_t bus);

    // 轮询一次。INT 无效时立刻返回，不碰 I2C。
    void Poll();

    // 取一份当前状态。
    //
    // **加锁是必要的**：写发生在触摸任务、读发生在仿真任务，两者在不同核上。
    // State 是十几个字节的聚合体，无保护的话读者可能拿到"新 x + 旧 y"这种
    // 撕裂组合 —— 一次坐标瞬移就会给液体注入一大笔能量。
    // 临界区只有一次十几字节的拷贝，开销可忽略。
    State Get();

    bool ready() const { return tp_ != nullptr; }

    // 诊断：真正读了几次 I2C。和轮询次数对比就知道 INT 门控有没有起作用。
    uint32_t reads() const { return reads_; }

 private:
    // 用 void* 而不是驱动类型，免得把 esp_lcd_touch 的头文件带进公开接口。
    void* tp_ = nullptr;
    void* io_ = nullptr;

    State state_;
    uint32_t reads_ = 0;
    int64_t last_us_ = 0;
    // 最后一次"解出有效点"的时刻（us）。松手判定以它为准 ——
    // **读失败不等于松手**，详见 touch.cpp 里的 kReleaseTimeoutUs。
    int64_t last_valid_us_ = 0;
    void MaybeRelease(int64_t now_us);

    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace touch
}  // namespace fluidbox
