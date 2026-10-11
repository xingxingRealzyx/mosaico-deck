// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// Compile-time configuration for mosaico-deck.
//
// Relationship to mosaico-fluidbox
// --------------------------------
// Display (band double buffering + TE sync), touch (including the "a bad report is not a
// release" debounce), and board revision detection (eFuse) were carried over directly
// from fluidbox -- all of them have been verified on real hardware, including the
// pitfalls that were hit. Only the parameters they need are kept here; everything
// simulation-related was removed.
//
// What this project does
// ----------------------
// A **desktop agent status/interaction terminal**: a 480x480 screen + touch + buttons +
// USB. The UI is **not rendered on the board** -- the PC renders it and sends it band by
// band, and the board only handles display and input reporting. So there is no layout,
// font, or color configuration here; all of that lives on the PC.

#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace fluidbox {
namespace config {

// ---------------------------------------
// Screen
// ---------------------------------------
inline constexpr int kLcdHRes = 480;
inline constexpr int kLcdVRes = 480;

// QSPI pixel clock.
//
// **60 MHz, not the official 40, and not the 80 it could run at.**
// The panel's refresh rate (TE measured about 58.5 Hz) is the real ceiling of this
// system: feeding a full 60 Hz needs about 54 MHz, and anything higher is pure waste.
// Moreover, at 80 MHz the edges do not have time to settle and occasional bit errors
// occur -- the symptom is "one band flashes a different color", and since QSPI is a
// write-only bus, the host side sees no error at all.
inline constexpr int kLcdPixelClockHz = CONFIG_FBOX_DISPLAY_PCLK_MHZ * 1000 * 1000;

// GPIO drive strength of the five QSPI lines. The official default is the weakest step
// (0, to reduce EMI), but its comment also states "turn it up if signal integrity fails at
// high clock". Here we use 2.
inline constexpr int kLcdDriveCap = CONFIG_FBOX_LCD_DRIVE_CAP;

// Brightness percentage (0~100). The panel is AMOLED, and pure black emits no light, so
// power saving depends mainly on the picture itself.
inline constexpr int kDisplayBrightness = 90;

// -- band partitioning --------------------------
//
// The screen is sliced into horizontal bands, each with its own buffer. This way:
//   - No single 450KB frame buffer is needed (internal SRAM cannot hold it)
//   - DMA transfers and CPU drawing can be pipelined and overlapped
//   - **The PC can send only the bands that changed** -- this is the key to how this
//     project saves bandwidth
inline constexpr int kBandRows = 30;
inline constexpr int kBandCount = kLcdVRes / kBandRows;
inline constexpr int kBandPixels = kLcdHRes * kBandRows;

static_assert(kBandCount * kBandRows == kLcdVRes,
              "the band row count must divide the screen height, otherwise the last band drops a few rows");

// ---------------------------------------
// Touch (used only as a "button matrix" in this project; no force feedback needed)
// ---------------------------------------
inline constexpr bool kTouchEnable = true;

// Low-pass time constant for finger velocity.
//
// **The low-pass is mandatory**: touch coordinates have quantization jitter, and
// differentiating them directly yields a fake velocity of several hundred px/s.
// This project only uses the coordinates (to tell which button was hit), so velocity is
// not actually needed, but the touch component computes it uniformly anyway -- kept for
// "swipe to switch pages" later.
inline constexpr float kTouchVelLpfTauS = 0.045f;

// Upper bound on finger velocity, to keep a single jump from producing absurd values.
inline constexpr float kTouchMaxSpeedPxS = 3500.0f;

// Touch polling period (ms). 3ms is far faster than the CST9220 report rate, so no events
// are missed.
inline constexpr int kTouchPollPeriodMs = 3;

// ---------------------------------------
// USB link (PC <-> board)
// ---------------------------------------
// Frame buffer upper bound: one screen is at most 480x480x2 = 450KB. The PC can send a
// whole screen (about 450ms) or only the bands that changed (a few KB, a few tens of ms).
// We reserve for a full screen here.
inline constexpr int kLinkFrameMaxBytes = kLcdHRes * kLcdVRes * 2;

// Upper bound on the length of one line of JSON (protocol messages are all single-line).
inline constexpr int kLinkLineMaxBytes = 512;

// ---------------------------------------
// Status statistics
// ---------------------------------------
inline constexpr int kStatsPeriodMs = 2000;

}  // namespace config
}  // namespace fluidbox
