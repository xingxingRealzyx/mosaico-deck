// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// ESP-Mosaico (ESP32-S31) 板级引脚定义。
//
// 来源：ESP-Mosaico V1.0 / V1.2 用户指南的「MCU 与管脚分配」表，
// 以及官方 BSP（esp-mosaico-bsp）的 include/bsp/esp_mosaico.h。
//
// 两个版本共用绝大多数引脚，只有 **LCD_RST/LCD_SCL 互换**、I2C0 换脚，
// 以及 V1.2 取消了橙色状态灯。
//
// ## 版本必须靠 eFuse 自动识别，不能猜
//
// 硬件版本烧在芯片 eFuse 的 USER_DATA 低 16 位里（高字节 major、低字节 minor），
// 官方 BSP 的 `bsp_board_variant_get()` 就是这么读的。
//
// **接反 LCD_RST 和 LCD_SCL 的后果特别隐蔽**：时钟打在复位脚上、复位脉冲打在
// 时钟脚上，面板永远收不到有效命令 —— 而 QSPI 是**只写总线**，ESP 侧完全不知情，
// 驱动一路 `ESP_OK`，最后得到一个"初始化成功但全程黑屏"的屏幕。本工程实际踩过
// 这个坑，靠 menuconfig 猜版本猜错了。
//
// 所以这里照官方做：`board::Init()` 启动时读 eFuse 自动选表，
// `CONFIG_FBOX_BOARD_V10/V12` 只在 eFuse 读不出来时兜底。

#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace fluidbox {
namespace board {

// ---------------------------------------------------------------------------
// 屏幕供电 —— 最容易踩的坑
// ---------------------------------------------------------------------------
// kPinVcc3V3Ctrl 控制屏幕供电及对外 3.3V/5V 输出，**低电平有效**。
// 上电时它是关的，必须主动拉低，否则面板没有任何反应。
inline constexpr int kPinVcc3V3Ctrl = 60;

// 开关机请求。正常运行时保持高阻；拉低（开漏）请求整机关机。
// 本工程永远不碰它。
inline constexpr int kPinPowerSwitch = 57;

// ---------------------------------------------------------------------------
// CO5300 QSPI AMOLED (480 x 480)
// ---------------------------------------------------------------------------
inline constexpr int kPinLcdCs = 50;
inline constexpr int kPinLcdData0 = 36;
inline constexpr int kPinLcdData1 = 51;
inline constexpr int kPinLcdData2 = 35;
inline constexpr int kPinLcdData3 = 9;
inline constexpr int kPinLcdTe = 43;  // 防撕裂同步（本工程不用）

// 上面这些两个版本一样，可以直接编进常量。下面这些**随版本变**，必须运行时选。

// ---------------------------------------------------------------------------
// 板子版本（eFuse 决定）
// ---------------------------------------------------------------------------
enum class Revision : uint8_t {
    kV10 = 0,
    kV12,
};

// 随版本变化的那几根脚。
struct Pins {
    int lcd_rst;
    int lcd_pclk;    // QSPI 时钟
    int i2c_sda;
    int i2c_scl;
    bool has_status_led;
    const char* name;
};

// 读 eFuse 决定用哪张表。必须在任何外设初始化**之前**调用。
// eFuse 读不出来（或值不支持）时退回 CONFIG_FBOX_BOARD_V10/V12，并打警告。
void Init();

// 当前生效的引脚表。Init() 之前调用会得到 Kconfig 指定的那张。
const Pins& Active();

Revision Detected();

// eFuse 里的原始版本字：0x0100 = v1.0，0x0101/0x0102 = v1.2。0 表示读不到。
uint16_t RawVersion();

// eFuse 是否给出了可信答案。false 表示 Init() 退回了 Kconfig 兜底值。
bool EfuseReliable();

// eFuse 不可信时的**第二路独立信号**：用 I2C 探测反推版本。
//
// 依据：V1.0 的对内 I2C0 在 GPIO0/1，V1.2 在 GPIO56/3。哪一组能扫到器件，
// 板子就是哪个版本。调用方（main 里的引脚排查）本来就要扫这两组，顺手把结果
// 报过来即可。
//
// **只在 eFuse 不可信时才生效** —— eFuse 是权威来源，不能被探测结果覆盖。
void SetRevisionFromI2cProbe(bool devices_on_v12_pins);

// ---------------------------------------------------------------------------
// I2C（内部总线：触摸 / IMU / 磁力计 / 电量计 / 音频 codec）
// ---------------------------------------------------------------------------
inline constexpr int kI2cPort = 0;
inline constexpr uint32_t kI2cFreqHz = 400000;

// I2C 从机地址
inline constexpr uint8_t kAddrBmi270 = 0x69;
inline constexpr uint8_t kAddrBmm150A = 0x11;
inline constexpr uint8_t kAddrBmm150B = 0x12;
inline constexpr uint8_t kAddrEs8311 = 0x19;
inline constexpr uint8_t kAddrBq27220 = 0x55;
inline constexpr uint8_t kAddrCst9220 = 0x5A;  // 触摸

// ---------------------------------------------------------------------------
// 人机交互
// ---------------------------------------------------------------------------
// 应用按键（AI 按键），低电平有效。按一下重置流体。
inline constexpr int kPinButtonAi = 7;
inline constexpr int kPinSensorInt = 2;  // IMU 中断（本工程靠轮询，不用）
inline constexpr int kPinTouchInt = 6;

// 程序可控橙色状态灯，低电平点亮，**仅 V1.0 有**。
// V1.2 取消了它并把该脚挪去当 I2C0_SCL —— 所以绝对不能无条件驱动 GPIO3，
// 那会把 I2C 时钟拉死。用 Active().has_status_led 判断。
inline constexpr int kPinStatusLed = 3;

inline constexpr int kPinMotor = 8;  // 振动马达，高电平开启（本工程不用）

}  // namespace board
}  // namespace fluidbox
