// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// touch.h -- CST9220 capacitive touch
//
// Purpose: let the user apply force to the particles with a finger.
//
// Design points
// -------------
// 1. **INT gating**. The underlying `esp_lcd_touch_read_data()` reads I2C
//    unconditionally, and this bus is shared with the IMU (IMU at 100 Hz). So we
//    watch the INT pin ourselves and only touch the bus when there really is a
//    touch event.
// 2. **The driver does not touch INT**. `int_gpio_num` is passed
//    `GPIO_NUM_NC` to avoid fighting with the logic above that watches the pin
//    itself (the driver would otherwise configure a GPIO interrupt by default).
// 3. **Coordinates are not flipped**. The official BSP's baseline at rotation 0
//    is exactly `swap_xy=false, mirror_x=false, mirror_y=false`, and this project
//    always uses rotation 0.

#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace fluidbox {
namespace touch {

// Finger state. The coordinates are **panel pixels**, the same coordinate system
// as the simulation/render screen coordinates.
struct State {
    bool active = false;
    int x = 0;
    int y = 0;
    float vx = 0.0f;  // pixels/second, already low-pass filtered
    float vy = 0.0f;
};

class Touchscreen {
 public:
    Touchscreen() = default;
    Touchscreen(const Touchscreen&) = delete;
    Touchscreen& operator=(const Touchscreen&) = delete;

    // Create the touch device on the given I2C bus. The bus must already be set
    // up (on Mosaico it is shared with the IMU on GPIO56/3).
    esp_err_t Init(i2c_master_bus_handle_t bus);

    // Poll once. Returns immediately without touching I2C when INT is inactive.
    void Poll();

    // Take a copy of the current state.
    //
    // **The lock is necessary**: writes happen in the touch task and reads happen
    // in the simulation task, and the two run on different cores. State is a
    // dozen-byte aggregate, and without protection a reader could get a torn
    // combination such as "new x + old y" -- a single coordinate jump injects a
    // large amount of energy into the fluid.
    // The critical section is just one dozen-byte copy, so the cost is negligible.
    State Get();

    bool ready() const { return tp_ != nullptr; }

    // Diagnostics: how many I2C reads actually happened. Comparing this with the
    // poll count shows whether INT gating is working.
    uint32_t reads() const { return reads_; }

 private:
    // void* instead of the driver types, so that the esp_lcd_touch headers do not
    // leak into the public interface.
    void* tp_ = nullptr;
    void* io_ = nullptr;

    State state_;
    uint32_t reads_ = 0;
    int64_t last_us_ = 0;
    // Timestamp (us) of the last "valid point decoded". The release decision is
    // based on it -- **a failed read does not mean release**, see
    // kReleaseTimeoutUs in touch.cpp.
    int64_t last_valid_us_ = 0;
    void MaybeRelease(int64_t now_us);

    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace touch
}  // namespace fluidbox
