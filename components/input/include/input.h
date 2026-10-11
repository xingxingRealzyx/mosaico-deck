// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// input -- the "reset fluid" button
//
// Why not the original project's button driver
// --------------------------------------------
// V4C38/esp32-fluidbox's button.c reads the PWR key from a TCA9554 IO expander;
// the ESP-Mosaico has no such chip, and the only on-board option is the AI button
// wired to GPIO7 (active low). So the original driver's short-press/long-press
// decision behaviour is kept here, and only the low-level layer is swapped from
// the I2C expander to a plain GPIO + internal pull-up.
//
// Why polling instead of an interrupt
// -----------------------------------
// The button is on GPIO7 and is a mechanical contact by nature; filtering the
// bounce with a software window is enough, and the main loop already ticks at
// about 25 ms. An extra interrupt would only add another concurrent path. Hence
// GPIO_INTR_DISABLE, with the caller invoking Poll() periodically.

#pragma once

#include "esp_err.h"

namespace input {

class ResetButton {
 public:
    ResetButton() = default;
    ~ResetButton() = default;

    ResetButton(const ResetButton&) = delete;
    ResetButton& operator=(const ResetButton&) = delete;

    // Configure the GPIO and enable the internal pull-up.
    esp_err_t Init();

    // The events a single sample can report.
    enum class Event : uint8_t {
        kNone,
        kShortPress,  // released within 1.5 seconds of being pressed
        kLongPress,   // held longer than 1.5 seconds -- **reported while still held**, no need to wait for the release
    };

    // Call periodically (about once every 25 ms).
    //
    // **This one must be the only entry point; do not split it into two
    // functions.**
    // We hit this once: there used to be TakeShortPress() and TakeLongPress(),
    // each maintaining the same debounce state (stable_ / raw_pressed_). When
    // called in sequence every round, the second one saw state the first one had
    // already modified -- the debounce state machine fell apart and both long
    // press and short press **stopped working**. The symptom is exactly what the
    // user described as "no response".
    //
    // Long press is reported while the button is still held (without waiting for
    // the release): this tier is meant for "press and try it out" (long press
    // toggles the touch force, and the hand can immediately go feel the new force
    // on the screen); if feedback waited for the release, the person would already
    // have left the screen.
    Event Poll();

    // Whether it is currently held (the debounced level).
    //
    // "Hold to talk" needs to know about the **continuous hold**, not the two
    // edges of press/release -- implementing that with the Poll() event stream
    // would require tracking state on the PC side and also handling dropped
    // events.
    bool IsPressed() const { return raw_pressed_; }

 private:
    bool raw_pressed_ = false;   // debounced level
    bool stable_ = false;
    int64_t press_start_us_ = 0;
    bool reported_ = false;
    bool long_reported_ = false;  // whether a long press has already been reported for this hold
};

}  // namespace input
