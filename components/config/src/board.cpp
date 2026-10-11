// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// board.cpp -- read the hardware revision from eFuse and select the pin table
//
// Why auto-detection is mandatory
// -------------------------------
// **LCD_RST / LCD_SCL are swapped** between V1.0 and V1.2. Wiring them backwards produces
// no error at all: QSPI is a write-only bus, so the ESP side pushes the command out and
// considers it a success, the driver returns `ESP_OK` the whole way, and the result is a
// screen that "initialized successfully but is black the entire time". This project
// guessed the revision through menuconfig, guessed wrong, and wasted a lot of time
// troubleshooting.
//
// The hardware revision is burned into the low 16 bits of the chip eFuse's USER_DATA
// (high byte major, low byte minor); the official BSP's `bsp_board_variant_get()`
// (esp-mosaico-bsp/onboard/esp_mosaico.c) reads it exactly this way:
//
//     esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &version, sizeof(version) * 8U);
//     case 0x0100 -> V1.0
//     case 0x0101 / 0x0102 -> V1.2
//
// We copy that set of criteria verbatim here rather than inventing our own.

#include "fbox/board.h"

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace fluidbox {
namespace board {
namespace {

constexpr const char* kTag = "board";

// In the official BSP's esp_mosaico.h:
//   BSP_LCD_RST_V1_0 = GPIO42  BSP_LCD_SCL_V1_0 = GPIO44
//   BSP_LCD_RST_V1_2 = GPIO44  BSP_LCD_SCL_V1_2 = GPIO42    <- swapped
//   I2C0 moves too: V1.0 on GPIO0/1, V1.2 on GPIO56/3
constexpr Pins kV10 = {42, 44, 0, 1, true, "V1.0"};
constexpr Pins kV12 = {44, 42, 56, 3, false, "V1.2"};

#if CONFIG_FBOX_BOARD_V10
constexpr Pins kFallback = kV10;
#else
constexpr Pins kFallback = kV12;
#endif

Pins g_active = kFallback;
Revision g_revision = Revision::kV10;
uint16_t g_raw_version = 0;
bool g_efuse_reliable = false;

void Apply(Revision rev, const char* why) {
    g_active = (rev == Revision::kV12) ? kV12 : kV10;
    g_revision = rev;
    ESP_LOGI(kTag, "board: %s (%s) LCD_RST=%d LCD_SCL=%d I2C SDA=%d SCL=%d status LED=%s",
             g_active.name, why, g_active.lcd_rst, g_active.lcd_pclk, g_active.i2c_sda,
             g_active.i2c_scl, g_active.has_status_led ? "yes" : "no");
}

}  // namespace

void Init() {
    // The official BSP's criteria, copied verbatim.
    const esp_err_t err =
        esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &g_raw_version,
                                  sizeof(g_raw_version) * 8U);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "failed to read hardware revision from eFuse: %s, temporarily using menuconfig's %s (awaiting I2C probe to correct)",
                 esp_err_to_name(err), kFallback.name);
        g_active = kFallback;
        g_revision =
            (kFallback.i2c_sda == kV10.i2c_sda) ? Revision::kV10 : Revision::kV12;
        g_efuse_reliable = false;
        return;
    }

    switch (g_raw_version) {
        case 0x0100:  // v1.0
            Apply(Revision::kV10, "eFuse says so");
            g_efuse_reliable = true;
            break;
        case 0x0101:  // v1.1
        case 0x0102:  // v1.2
            Apply(Revision::kV12, "eFuse says so");
            g_efuse_reliable = true;
            break;
        default:
            ESP_LOGW(kTag,
                     "hardware revision in eFuse raw=0x%04X is not a known version (0x0100/0x0101/0x0102), "
                     "temporarily using menuconfig's %s (awaiting I2C probe to correct)",
                     static_cast<unsigned>(g_raw_version), kFallback.name);
            g_active = kFallback;
            g_revision =
                (kFallback.i2c_sda == kV10.i2c_sda) ? Revision::kV10 : Revision::kV12;
            g_efuse_reliable = false;
            return;
    }

    ESP_LOGI(kTag, "board decision: eFuse raw=0x%04X -> %s",
             static_cast<unsigned>(g_raw_version), g_active.name);
}

bool EfuseReliable() { return g_efuse_reliable; }

void SetRevisionFromI2cProbe(bool devices_on_v12_pins) {
    if (g_efuse_reliable) {
        return;  // the eFuse is the authoritative source; probe results must not override it
    }
    Apply(devices_on_v12_pins ? Revision::kV12 : Revision::kV10, "eFuse untrustworthy, decided by I2C probe");
}

const Pins& Active() { return g_active; }

Revision Detected() { return g_revision; }

uint16_t RawVersion() { return g_raw_version; }

}  // namespace board
}  // namespace fluidbox
