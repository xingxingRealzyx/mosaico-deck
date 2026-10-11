// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// link.h —— PC ↔ 板子的 USB 链路
//
// 协议
// ----
// PC → 板子：**二进制帧**（band 像素是 28800 字节，塞不进 JSON）
//
//     ┌────────┬──────┬───────────┬──────────────┐
//     │ magic  │ type │ len (4,LE)│ payload      │
//     │ DE AD  │  1B  │           │ len 字节     │
//     └────────┴──────┴───────────┴──────────────┘
//
//     type 0x01 = 一条 band：payload = idx(1B) + 480×30×2 像素（RGB565，已转字节序）
//     type 0x02 = 整屏：    payload = 480×480×2 像素
//     type 0x10 = 控制：    payload = 一行 JSON（UTF-8，不带换行）
//
// 板子 → PC：**JSON 行**（都是小消息，可读性比省几个字节重要）
//
//     {"t":"hello","fw":"...","bands":16}
//     {"t":"touch","ev":"down","x":123,"y":456}
//     {"t":"touch","ev":"up"}
//     {"t":"key","ev":"short"|"long"}
//     {"t":"ack","band":3}
//     {"t":"err","msg":"..."}
//
// 为什么板子侧**不需要帧缓冲**
// ------------------------
// 收到一条 band 就直接写进那条 band 的缓冲、推出去。于是：
//   · 部分更新天然支持 —— PC 只发变化的 band，没发的那些面板上原样保留
//   · 板子不需要知道 UI 长什么样，它就是个哑终端
//   · 28KB 的 payload 是**流式**收进 band 缓冲的，不经过中转缓冲

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace link {

// 注入要驱动的面板。**必须在 Init() 之前调用** —— link 不认识面板从哪来，
// 由 main 决定（这样 link 也不依赖 display 的具体构造方式）。
// 传 nullptr 合法：面板没就绪时 link 照常收帧、只是不显示（否则流会错位）。
void SetPanel(void* panel);

// 建 RX 任务并挂上输入上报。USB 控制台必须先 Init()。
esp_err_t Init();

// 上报输入。由触摸/按键任务调用 —— 内部会加锁，可以多任务并发。
void SendTouch(bool down, int x, int y);
void SendKey(const char* ev);

// PC 是否已经送过帧。
//
// **屏幕的所有权只能有一个主人。** 本地自检图案和 PC 送来的帧如果同时画，
// 两个任务会抢 SPI 总线（实测报 `acquire spi bus failed` + `ESP_ERR_INVALID_STATE`），
// 而且消费任务被总线卡住之后就没空读环形缓冲 —— 表现为大量 RX 溢出。
//
// 所以 main 里的渲染任务用它来判断：PC 一旦接管，本地图案就停。
bool RemoteOwnsScreen();

// 收到过 / 丢过多少。诊断用。
struct Stats {
    uint32_t frames = 0;    // 解析成功的帧数
    uint32_t bands = 0;     // 收到的 band 数
    uint32_t full = 0;      // 收到的整屏数
    uint32_t ctrl = 0;      // 收到的控制消息数
    uint32_t bad_magic = 0;  // 帧头对不上（通常意味着前面丢了字节）
    uint32_t bad_len = 0;   // 长度越界
    uint32_t rx_timeout = 0;  // 收 payload 时超时（对方断流了）
};
Stats GetStats();

}  // namespace link
