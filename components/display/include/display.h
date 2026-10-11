// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// display —— CO5300 QSPI AMOLED 面板，band 双缓冲 + 异步 DMA
//
// 没有整帧缓冲
// ------------
// 480x480x2 = 450 KB 一帧，内部 SRAM 放不下两份；放 PSRAM 又会让 CPU 写像素和
// DMA 读像素抢同一条外部总线。
//
// 所以屏幕按 **16 条 30 行的水平 band** 来画，每条 28800 字节，两份能舒服地住在
// 内部 SRAM 里。循环是：
//
//     for 每条 band:
//         等这条 band 的缓冲区空出来
//         把它清成黑色
//         把所有和这条 band 相交的粒子画进去
//         交给 DMA，立刻继续下一条
//
// esp_lcd_panel_draw_bitmap() 只是把传输排进队列就返回，所以 CPU 在画第 N 条时
// 第 N-1 条还在线上跑。计数信号量初值 2、由传输完成中断释放，"等缓冲区空出来"
// 就是这么实现的：取到信号量意味着至少有一块空闲，而传输按排队顺序完成，
// 所以空闲的必然就是接下来要复用的那块。
//
// 面板链路是天花板。QSPI 每时钟 4 bit，一帧 450 KB 在 80 MHz 下约 11.3 ms。
// S31 的 GP-SPI 主机上限就是 80 MHz；官方 BSP 保守用了 40 MHz。

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "osal.h"

namespace display {

class Panel {
 public:
    Panel() = default;
    ~Panel();

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    // QSPI 总线 + 面板 IO + CO5300 初始化 + 开显示。
    esp_err_t Init();

    // 0..255。AMOLED 自己调亮度，没有背光引脚。
    esp_err_t SetBrightness(uint8_t level);

    // **显示不是线程安全的**：两个任务同时"取缓冲 → 画 → 送"会抢 SPI 总线，
    // 报 `acquire spi bus failed` / `ESP_ERR_INVALID_STATE`。
    //
    // 正常情况靠"屏幕所有权只有一个主人"来保证（PC 接管后本地图案就停），
    // 但过渡的那一瞬仍然可能撞上。这个锁是那一下的保险。
    //
    // 用法：DrawLock() 返回的守卫活过整段 Acquire → 画 → Flush。
    [[nodiscard]] osal::Mutex& DrawMutex() { return draw_mutex_; }

    // 阻塞直到有一块 band 缓冲空闲，然后返回它。
    // 大小是 kBandRows * kLcdHRes 像素。
    //
    // 缓冲按"申请次数"轮转而不是按 band 序号：渲染器会跳过在面板上已经是黑的
    // band，按序号取会把同一块缓冲交给两个还在飞的传输。
    //
    // **面板没就绪时不会返回空指针**，而是返回一块哑缓冲（渲染器照常画，
    // FlushBand 直接丢弃）。诊断模式 FBOX_SKIP_PANEL 走的正是这条路 ——
    // 要是这里解引用空指针，那条"救砖"路径本身就会崩，等于没有。
    //
    // 等待有超时：万一传输完成中断没回来（FlushBand 排队失败过），宁可丢一帧
    // 并打日志，也不能无限等下去冻屏。
    uint16_t* AcquireBand();

    // 排入一条 band 的异步传输。不阻塞。
    // 排队失败时会把缓冲还回去 —— 否则完成中断永远不来，那块缓冲就永久丢了，
    // 连丢两次之后 AcquireBand 会一直等不到。
    esp_err_t FlushBand(int band_index, const uint16_t* buffer);

    // 面板真的可用：Init 成功、有 panel 句柄、也有缓冲信号量。
    bool ready() const { return panel_ != nullptr && buffers_free_ != nullptr; }

    // 累计的 TE（撕裂同步）上升沿数。
    //
    // **这是唯一不靠人眼的"面板到底活没活"的证据。** QSPI 是只写总线 ——
    // 命令有没有被面板接受、面板有没有在扫描，主机从总线上完全看不出来。
    // 但 TE 是面板**输出**给主机的信号：只要面板上电、被正确初始化、
    // 并且正在扫描，它就会按刷新率持续出脉冲。读它就能把"面板死了"和
    // "面板活着只是我们没看到"区分开。
    //
    // 需要初始化时那条 0x35（TE ON）命令，以及 GPIO43 接成输入。
    uint32_t TeEdges() const;

    // 阻塞等到下一个 TE 上升沿，最多等 timeout_ms。返回 true 表示真等到了。
    //
    // **这是帧同步。** 面板的刷新率（约 58.5 Hz）就是这个系统的真实上限；
    // 推得比它快不会让画面更流畅，只会在面板正在扫描某片区域时往它的 GRAM
    // 里写 —— 那样**整条 band 可能整帧都没被接受**，看起来就是"某一条不刷新"。
    // 而且这种冲突在主机侧完全看不出来（QSPI 只写，驱动一路 ESP_OK）。
    //
    // 官方 BSP 也是这么做的（CONFIG_CO5300_ENABLE_TE + TE_SYNC）。
    bool WaitForTeTick(uint32_t timeout_ms);

    // 关显示 + 亮度归零，把 AMOLED 的电流降到最低。
    // 进 ROM 下载模式之前会调 —— 重启后 GPIO 状态会丢，ROM 也不驱动屏幕的
    // 3.3V 控制脚，屏幕要是还亮着，烧 flash 的时候就会和写入电流尖峰叠加。
    void PrepareForBootloader();

    // 缓冲等待上限。正常一次传输 28800 字节在 80 MHz 下约 1.4 ms，
    // 500 ms 已经远超"正常"，只可能是中断没回来。
    static constexpr uint32_t kAcquireTimeoutMs = 500;

 private:
    void* panel_ = nullptr;  // esp_lcd_panel_handle_t
    void* io_ = nullptr;     // esp_lcd_panel_io_handle_t
    void* buffers_free_ = nullptr;  // osal::CountingSemaphore*
    osal::Mutex draw_mutex_;
    unsigned next_buffer_ = 0;
};

}  // namespace display
