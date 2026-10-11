// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// board.cpp —— 从 eFuse 读硬件版本，选定引脚表
//
// 为什么必须自动识别
// ----------------
// V1.0 和 V1.2 的 **LCD_RST / LCD_SCL 是互换的**。接反了不会有任何报错：
// QSPI 是只写总线，ESP 侧把命令推出去就当成功了，驱动一路 `ESP_OK`，
// 结果是一个"初始化成功但全程黑屏"的屏幕。本工程就是靠 menuconfig 猜版本、
// 猜错了，白白排查了很久。
//
// 硬件版本烧在芯片 eFuse 的 USER_DATA 低 16 位里（高字节 major、低字节 minor），
// 官方 BSP 的 `bsp_board_variant_get()`（esp-mosaico-bsp/onboard/esp_mosaico.c）
// 就是这么读的：
//
//     esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &version, sizeof(version) * 8U);
//     case 0x0100 -> V1.0
//     case 0x0101 / 0x0102 -> V1.2
//
// 这里逐字照搬那套判据，不自己发明。

#include "fbox/board.h"

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace fluidbox {
namespace board {
namespace {

constexpr const char* kTag = "board";

// 官方 BSP 的 esp_mosaico.h 里：
//   BSP_LCD_RST_V1_0 = GPIO42  BSP_LCD_SCL_V1_0 = GPIO44
//   BSP_LCD_RST_V1_2 = GPIO44  BSP_LCD_SCL_V1_2 = GPIO42    <- 互换
//   I2C0 也跟着换：V1.0 在 GPIO0/1，V1.2 在 GPIO56/3
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
    ESP_LOGI(kTag, "板级: %s（%s）LCD_RST=%d LCD_SCL=%d I2C SDA=%d SCL=%d 状态灯=%s",
             g_active.name, why, g_active.lcd_rst, g_active.lcd_pclk, g_active.i2c_sda,
             g_active.i2c_scl, g_active.has_status_led ? "有" : "无");
}

}  // namespace

void Init() {
    // 官方 BSP 的判据，逐字照搬。
    const esp_err_t err =
        esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &g_raw_version,
                                  sizeof(g_raw_version) * 8U);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "读 eFuse 硬件版本失败: %s，暂用 menuconfig 的 %s（等 I2C 探测来纠正）",
                 esp_err_to_name(err), kFallback.name);
        g_active = kFallback;
        g_revision =
            (kFallback.i2c_sda == kV10.i2c_sda) ? Revision::kV10 : Revision::kV12;
        g_efuse_reliable = false;
        return;
    }

    switch (g_raw_version) {
        case 0x0100:  // v1.0
            Apply(Revision::kV10, "eFuse 认定");
            g_efuse_reliable = true;
            break;
        case 0x0101:  // v1.1
        case 0x0102:  // v1.2
            Apply(Revision::kV12, "eFuse 认定");
            g_efuse_reliable = true;
            break;
        default:
            ESP_LOGW(kTag,
                     "eFuse 里的硬件版本 raw=0x%04X 不是已知版本（0x0100/0x0101/0x0102），"
                     "暂用 menuconfig 的 %s（等 I2C 探测来纠正）",
                     static_cast<unsigned>(g_raw_version), kFallback.name);
            g_active = kFallback;
            g_revision =
                (kFallback.i2c_sda == kV10.i2c_sda) ? Revision::kV10 : Revision::kV12;
            g_efuse_reliable = false;
            return;
    }

    ESP_LOGI(kTag, "板级判定: eFuse raw=0x%04X -> %s",
             static_cast<unsigned>(g_raw_version), g_active.name);
}

bool EfuseReliable() { return g_efuse_reliable; }

void SetRevisionFromI2cProbe(bool devices_on_v12_pins) {
    if (g_efuse_reliable) {
        return;  // eFuse 是权威来源，探测结果不能覆盖它
    }
    Apply(devices_on_v12_pins ? Revision::kV12 : Revision::kV10, "eFuse 不可信，由 I2C 探测判定");
}

const Pins& Active() { return g_active; }

Revision Detected() { return g_revision; }

uint16_t RawVersion() { return g_raw_version; }

}  // namespace board
}  // namespace fluidbox
