// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// ESP-Mosaico (ESP32-S31) board-level pin definitions.
//
// Source: the "MCU and pin assignment" table in the ESP-Mosaico V1.0 / V1.2 user guide,
// and the official BSP (esp-mosaico-bsp) include/bsp/esp_mosaico.h.
//
// The two revisions share the vast majority of the pins; only **LCD_RST/LCD_SCL are
// swapped**, I2C0 moves pins, and V1.2 removed the orange status LED.
//
// ## The revision must be auto-detected via eFuse, never guessed
//
// The hardware revision is burned into the low 16 bits of the chip eFuse's USER_DATA
// (high byte major, low byte minor); the official BSP's `bsp_board_variant_get()` reads
// it exactly this way.
//
// **The consequence of swapping LCD_RST and LCD_SCL is especially insidious**: the clock
// lands on the reset pin and the reset pulse lands on the clock pin, so the panel never
// receives a valid command -- and since QSPI is a **write-only bus**, the ESP side is
// completely unaware: the driver returns `ESP_OK` the whole way, and you end up with a
// screen that "initialized successfully but is black the entire time". This project
// actually hit this pitfall by guessing the revision through menuconfig and guessing
// wrong.
//
// So we follow the official approach here: `board::Init()` reads the eFuse at startup and
// selects the table automatically, while `CONFIG_FBOX_BOARD_V10/V12` is only a fallback
// for when the eFuse cannot be read.

#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace fluidbox {
namespace board {

// ---------------------------------------
// Screen power -- the pitfall most easily stepped into
// ---------------------------------------
// kPinVcc3V3Ctrl controls the screen power and the external 3.3V/5V output, and is
// **active low**. At power-on it is off and must be actively pulled low, otherwise the
// panel does not respond at all.
inline constexpr int kPinVcc3V3Ctrl = 60;

// Power on/off request. Keep it high-impedance during normal operation; pulling it low
// (open drain) requests a full shutdown of the unit.
// This project never touches it.
inline constexpr int kPinPowerSwitch = 57;

// ---------------------------------------
// CO5300 QSPI AMOLED (480 x 480)
// ---------------------------------------
inline constexpr int kPinLcdCs = 50;
inline constexpr int kPinLcdData0 = 36;
inline constexpr int kPinLcdData1 = 51;
inline constexpr int kPinLcdData2 = 35;
inline constexpr int kPinLcdData3 = 9;
inline constexpr int kPinLcdTe = 43;  // tear-free sync (unused in this project)

// The pins above are the same on both revisions and can be compiled in as constants. The
// ones below **vary with the revision** and must be selected at runtime.

// ---------------------------------------
// Board revision (decided by eFuse)
// ---------------------------------------
enum class Revision : uint8_t {
    kV10 = 0,
    kV12,
};

// The pins that vary with the revision.
struct Pins {
    int lcd_rst;
    int lcd_pclk;    // QSPI clock
    int i2c_sda;
    int i2c_scl;
    bool has_status_led;
    const char* name;
};

// Read the eFuse to decide which table to use. Must be called **before** any peripheral is
// initialized.
// If the eFuse cannot be read (or holds an unsupported value), fall back to
// CONFIG_FBOX_BOARD_V10/V12 and log a warning.
void Init();

// The currently effective pin table. Calling this before Init() yields the table specified
// by Kconfig.
const Pins& Active();

Revision Detected();

// The raw revision word in the eFuse: 0x0100 = v1.0, 0x0101/0x0102 = v1.2. 0 means it
// could not be read.
uint16_t RawVersion();

// Whether the eFuse gave a trustworthy answer. false means Init() fell back to the Kconfig
// value.
bool EfuseReliable();

// **A second, independent signal** for when the eFuse is not trustworthy: infer the
// revision from an I2C probe.
//
// Basis: on V1.0 the internal I2C0 is on GPIO0/1, on V1.2 it is on GPIO56/3. Whichever
// group can see devices is the revision of the board. The caller (the pin
// troubleshooting in main) has to scan both groups anyway, so it can simply report the
// result here.
//
// **Only takes effect when the eFuse is not trustworthy** -- the eFuse is the authoritative
// source and must not be overridden by probe results.
void SetRevisionFromI2cProbe(bool devices_on_v12_pins);

// ---------------------------------------
// I2C (internal bus: touch / IMU / magnetometer / fuel gauge / audio codec)
// ---------------------------------------
inline constexpr int kI2cPort = 0;
inline constexpr uint32_t kI2cFreqHz = 400000;

// I2C slave addresses
inline constexpr uint8_t kAddrBmi270 = 0x69;
inline constexpr uint8_t kAddrBmm150A = 0x11;
inline constexpr uint8_t kAddrBmm150B = 0x12;
inline constexpr uint8_t kAddrEs8311 = 0x19;
inline constexpr uint8_t kAddrBq27220 = 0x55;
inline constexpr uint8_t kAddrCst9220 = 0x5A;  // touch

// ---------------------------------------
// Human-machine interaction
// ---------------------------------------
// Application button (AI button), active low. One press resets the fluid.
inline constexpr int kPinButtonAi = 7;
inline constexpr int kPinSensorInt = 2;  // IMU interrupt (this project polls instead, unused)
inline constexpr int kPinTouchInt = 6;

// Program-controllable orange status LED, lit by a low level, **only present on V1.0**.
// V1.2 removed it and moved that pin to I2C0_SCL -- so GPIO3 must absolutely not be driven
// unconditionally, as that would hold the I2C clock low forever. Use
// Active().has_status_led to decide.
inline constexpr int kPinStatusLed = 3;

inline constexpr int kPinMotor = 8;  // vibration motor, enabled by a high level (unused in this project)

}  // namespace board
}  // namespace fluidbox
