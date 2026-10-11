// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// usb_console —— USB-OTG CDC 控制台 + 免按键烧录
//
// 为什么不用 USB Serial/JTAG
// --------------------------
// ESP32-S31 有两个 USB 外设，且不能同时工作（乐鑫官方 FAQ）：
//   - USB-OTG          <- ESP-Mosaico 的 Type-C 接的是这个
//   - USB Serial/JTAG  <- 引到了左侧模块排针 H2 的 13/15 脚
// ROM 下载模式走的也是 OTG（esptool 会把它识别成 "USB mode: USB-OTG"，
// VID 0x303A / PID = 芯片 ID）。所以想让 Type-C 上的虚拟串口支持免按键烧录，
// 必须在应用层模拟 ROM CDC 的行为。
//
// 免按键烧录的原理
// ----------------
// esptool 复位芯片时会在 CDC 控制线上打出这个时序：
//
//     DTR=0, RTS=1  ->  DTR=1, RTS=0  ->  DTR=0, RTS=0
//                        ^^^^^^^^^^^^
//                        关键：RTS 下降沿时 DTR 为高
//
// 我们抓住这个沿，置位 S31 的 LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT 再重启，
// ROM 起来后直接进下载模式 —— 全程不用按 BOOT。
//
// 描述符必须和 ROM 一致
// --------------------
// 应用态和 ROM 下载态使用同一套 VID/PID/字符串，macOS 才会分配同一个
// /dev/cu.usbmodemXXXX，刷写脚本才能在复位前后拿到同一个端口名。

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace usb_console {

// 安装 TinyUSB + CDC-ACM，注册 VFS，并挂上控制线回调。
// 调用后即可用 Printf() 输出，或把 /dev/tusb_cdc 当普通串口用。
esp_err_t Init();

// 主机是否已经打开 CDC。
bool Connected();

// 直接写 CDC。
//
// ⚠️ **没有互斥保护**：底层 tinyusb_cdcacm_write_queue / tud_cdc_n_write 都不带锁。
// 多个任务同时调 Write()/Printf() 会交错。目前调用方只有主任务和统计日志，
// 实际上没并发；将来要多任务用，这里得先加一把 osal::Mutex。
size_t Write(const uint8_t* data, size_t len);
size_t Write(const char* text);

// printf 风格输出。
void Printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// 把 ESP-IDF 的日志重定向到 CDC。
void AttachToLogger();

// 重启进 ROM 下载模式之前的回调。
//
// **为什么需要它**：重启之后 GPIO 的状态会丢，而 ROM 不驱动屏幕的 3.3V 控制脚。
// 如果这时 AMOLED 还亮着，整个烧录过程都在额外耗电 —— 而写 flash 本身有电流
// 尖峰，叠加起来就可能把供电拉崩，表现是"烧到一半端口就没了"。
// 注册一个把屏幕关掉的钩子，能把这个变量消掉。
using PreRestartHook = void (*)();
void SetPreRestartHook(PreRestartHook hook);

// 立刻重启进 ROM 下载模式（等价于 esptool 的握手）。
// 正常流程下由控制线回调自动触发，这个接口留给手动命令用。
void RebootToBootloader();

}  // namespace usb_console
