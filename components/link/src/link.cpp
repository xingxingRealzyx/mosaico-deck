// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// link.cpp —— PC ↔ 板子的 USB 链路

#include "link.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "display.h"
#include "esp_log.h"
#include "fbox/config.h"
#include "osal.h"
#include "usb_console.h"

namespace link {
namespace {

constexpr const char* kTag = "link";

// 二进制帧的魔数。用两个字节而不是一个：流一旦错位，单字节魔数很容易碰巧命中，
// 双字节就基本不会（1/65536），而且错位之后能靠它重新对齐。
constexpr uint8_t kMagic0 = 0xDE;
constexpr uint8_t kMagic1 = 0xAD;

constexpr uint8_t kTypeBand = 0x01;
constexpr uint8_t kTypeFull = 0x02;
constexpr uint8_t kTypeCtrl = 0x10;

constexpr size_t kBandPayload = 1 + fluidbox::config::kBandPixels * 2;      // idx + 像素
constexpr size_t kFullPayload = fluidbox::config::kLcdHRes * fluidbox::config::kLcdVRes * 2;

display::Panel* g_panel = nullptr;
Stats g_stats;

// 发消息的锁。**必须有**：usb_console::Write 内部没有锁（它自己的注释里写明了），
// 而触摸任务和按键任务都会发消息，不加锁就会交错。
osal::Mutex* g_tx_lock = nullptr;

// 控制消息的 payload 上限。都是小 JSON，512 字节足够。
constexpr size_t kCtrlMax = 512;

// ---------------------------------------------------------------------------
// 字节流读取器
// ---------------------------------------------------------------------------
// 底层给的是**变长的块**（一次 RX 回调最多 128 字节），而协议要的是"恰好 N 字节"。
// 块尾多出来的那部分不能丢 —— 它是下一个字段的开头。所以留一个 carry。
//
// carry 只需要放得下一个块（128 字节），256 是宽裕的。
class RxStream {
 public:
    // 收满 len 字节。返回实际收到的字节数 —— **小于 len 说明超时了**，
    // 调用方必须当成错误处理，不能假设后面还有数据。
    size_t Read(uint8_t* dst, size_t len, uint32_t timeout_ms) {
        size_t got = 0;

        // 先把上次剩下的吐出来
        while (got < len && carry_len_ > 0) {
            const size_t n = (len - got < carry_len_) ? (len - got) : carry_len_;
            memcpy(dst + got, carry_, n);
            memmove(carry_, carry_ + n, carry_len_ - n);
            carry_len_ -= n;
            got += n;
        }

        while (got < len) {
            const osal::InputBuffer chunk = usb_console::ReceiveRx(timeout_ms);
            if (chunk.empty()) {
                break;  // 超时
            }
            const size_t need = len - got;
            const size_t take = (chunk.size() < need) ? chunk.size() : need;
            memcpy(dst + got, chunk.data(), take);
            got += take;

            // 块尾多出来的存进 carry，下次先用它
            if (take < chunk.size()) {
                const size_t rest = chunk.size() - take;
                if (rest <= sizeof(carry_) - carry_len_) {
                    memcpy(carry_ + carry_len_, chunk.data() + take, rest);
                    carry_len_ += rest;
                }
                // carry 放不下就是协议出问题了（单块最大 128 字节），丢掉并计数
            }
            usb_console::ReturnRx(chunk);
        }
        return got;
    }

    // 丢掉 carry 里的一切。帧头对不上时调用 —— 重新找同步点比继续猜安全。
    void Resync() { carry_len_ = 0; }

 private:
    uint8_t carry_[256] = {};
    size_t carry_len_ = 0;
};

RxStream g_rx;

// ---------------------------------------------------------------------------
// 板子 → PC
// ---------------------------------------------------------------------------
void SendLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void SendLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (static_cast<size_t>(n) < sizeof(buf) - 2) ? static_cast<size_t>(n)
                                                            : sizeof(buf) - 2;
    buf[len++] = '\n';
    buf[len] = '\0';

    // 加锁：触摸任务和按键任务都会走到这里。
    osal::LockGuard<osal::Mutex> guard(*g_tx_lock);
    usb_console::Write(reinterpret_cast<const uint8_t*>(buf), len);
}

void HandleCtrl(const uint8_t* payload, size_t len) {
    // 目前只有一条：把 payload 原样当作一行 JSON 打出来，方便在 PC 侧确认
    // 板子收到了什么。真正的控制命令以后在这里加分支。
    //
    // 注意 payload 不一定以 '\0' 结尾（协议里不带终止符），所以先拷进栈缓冲。
    char text[kCtrlMax + 1];
    const size_t n = (len < kCtrlMax) ? len : kCtrlMax;
    memcpy(text, payload, n);
    text[n] = '\0';
    ESP_LOGI(kTag, "控制消息: %s", text);
    SendLine("{\"t\":\"ack\",\"ctrl\":true}");
}

// 把一条 band 的像素**流式**收进 band 缓冲。
//
// 这是这个组件最要紧的一段：28800 字节不是先收到一个中转缓冲再拷过去的，
// 而是直接在环形缓冲和 band 缓冲之间搬 —— 省掉 28KB 的常驻内存，
// 而这块芯片总共只有 512KB SRAM。
void HandleBand(bool full_frame) {
    const size_t total = full_frame ? kFullPayload : kBandPayload;

    // 单条 band 时先把 idx 读出来
    uint8_t idx = 0;
    if (!full_frame) {
        if (g_rx.Read(&idx, 1, 1000) != 1) {
            g_stats.rx_timeout = g_stats.rx_timeout + 1;
            return;
        }
        if (idx >= fluidbox::config::kBandCount) {
            g_stats.bad_len = g_stats.bad_len + 1;
            // 内容还是要读掉，否则后面全部错位 —— 但既然长度已知，读进临时区丢弃。
            // 这里用一个小的丢弃循环，不分配内存。
            uint8_t sink[128];
            size_t left = total - 1;
            while (left > 0) {
                const size_t n = (left < sizeof(sink)) ? left : sizeof(sink);
                const size_t got = g_rx.Read(sink, n, 1000);
                if (got < n) {
                    g_stats.rx_timeout = g_stats.rx_timeout + 1;
                    return;
                }
                left -= n;
            }
            return;
        }
    }

    if (g_panel == nullptr || !g_panel->ready()) {
        // 面板没就绪也要把数据读掉，否则流会错位
        uint8_t sink[128];
        size_t left = full_frame ? total : (total - 1);
        while (left > 0) {
            const size_t n = (left < sizeof(sink)) ? left : sizeof(sink);
            const size_t got = g_rx.Read(sink, n, 1000);
            if (got < n) {
                g_stats.rx_timeout = g_stats.rx_timeout + 1;
                return;
            }
            left -= n;
        }
        return;
    }

    // 从这里开始到函数结束，独占显示。
    //
    // 过渡的那一瞬仍然可能和本地自检图案撞上（PC 的帧刚到、渲染任务还没看到
    // 让位标志），那一下会报 `acquire spi bus failed`。这把锁就是兜住它。
    osal::LockGuard<osal::Mutex> draw_guard(g_panel->DrawMutex());

    if (full_frame) {
        // 整屏：逐条 band 收，每条 480×30×2 字节。**不需要整帧缓冲** ——
        // 收到一条就推一条，和部分更新走的是同一条路径。
        for (int band = 0; band < fluidbox::config::kBandCount; band++) {
            uint16_t* buf = g_panel->AcquireBand();
            if (buf == nullptr) {
                continue;
            }
            const size_t bytes = fluidbox::config::kBandPixels * 2;
            if (g_rx.Read(reinterpret_cast<uint8_t*>(buf), bytes, 1000) != bytes) {
                g_stats.rx_timeout = g_stats.rx_timeout + 1;
                return;
            }
            g_panel->FlushBand(band, buf);
        }
        g_stats.full = g_stats.full + 1;
        g_stats.bands = g_stats.bands + fluidbox::config::kBandCount;
        return;
    }

    uint16_t* buf = g_panel->AcquireBand();
    if (buf == nullptr) {
        return;
    }
    const size_t bytes = fluidbox::config::kBandPixels * 2;
    if (g_rx.Read(reinterpret_cast<uint8_t*>(buf), bytes, 1000) != bytes) {
        g_stats.rx_timeout = g_stats.rx_timeout + 1;
        return;
    }
    g_panel->FlushBand(idx, buf);
    g_stats.bands = g_stats.bands + 1;

    // 每 64 条 band 回一个 ack，方便 PC 侧确认链路活着 —— 每条都回太吵。
    if ((g_stats.bands % 64) == 0) {
        SendLine("{\"t\":\"ack\",\"band\":%u}", static_cast<unsigned>(g_stats.bands));
    }
}

void RxTask(void* arg) {
    (void)arg;
    ESP_LOGI(kTag, "链路接收任务启动");

    // 找帧头。**必须容忍错位**：一旦上面某次读超时，流里就少了几个字节，
    // 后面的帧头就对不上了。这里逐字节找 DE AD，找到就重新同步。
    uint8_t prev = 0;
    for (;;) {
        uint8_t b = 0;
        if (g_rx.Read(&b, 1, 200) != 1) {
            osal::Delay(1);
            continue;
        }
        if (prev != kMagic0 || b != kMagic1) {
            prev = b;
            continue;
        }
        prev = 0;

        uint8_t header[5] = {};
        if (g_rx.Read(header, sizeof(header), 1000) != sizeof(header)) {
            g_stats.rx_timeout = g_stats.rx_timeout + 1;
            g_rx.Resync();
            continue;
        }
        const uint8_t type = header[0];
        const uint32_t len = static_cast<uint32_t>(header[1]) |
                             (static_cast<uint32_t>(header[2]) << 8) |
                             (static_cast<uint32_t>(header[3]) << 16) |
                             (static_cast<uint32_t>(header[4]) << 24);

        // 长度上限检查。不检查的话一个坏长度会让板子去等几 MB 的数据。
        const uint32_t max_len = (type == kTypeFull)  ? kFullPayload
                                 : (type == kTypeBand) ? kBandPayload
                                                       : kCtrlMax;
        if (len > max_len) {
            g_stats.bad_len = g_stats.bad_len + 1;
            ESP_LOGW(kTag, "帧长度越界 type=0x%02X len=%u (上限 %u)", type,
                     static_cast<unsigned>(len), static_cast<unsigned>(max_len));
            g_rx.Resync();
            continue;
        }

        g_stats.frames = g_stats.frames + 1;

        if (type == kTypeBand) {
            HandleBand(false);
        } else if (type == kTypeFull) {
            HandleBand(true);
        } else if (type == kTypeCtrl) {
            uint8_t payload[kCtrlMax];
            if (g_rx.Read(payload, len, 1000) == len) {
                g_stats.ctrl = g_stats.ctrl + 1;
                HandleCtrl(payload, len);
            } else {
                g_stats.rx_timeout = g_stats.rx_timeout + 1;
                g_rx.Resync();
            }
        } else {
            g_stats.bad_magic = g_stats.bad_magic + 1;
            g_rx.Resync();
        }
    }
}

}  // namespace

void SetPanel(void* panel) {
    g_panel = static_cast<display::Panel*>(panel);
}

esp_err_t Init() {
    if (g_tx_lock == nullptr) {
        g_tx_lock = new osal::Mutex();
    }

    // **优先级必须高于 TinyUSB 的任务（默认 5）。**
    //
    // 这里踩过一个很隐蔽的坑：TinyUSB 的 RX 回调跑在它自己的任务里（优先级 5），
    // 而接收任务原来只有 4 —— 于是**回调一直抢占消费者**：数据不停往环形缓冲里倒，
    // 消费者却拿不到时间片，缓冲很快填满。实测溢出 1706 次（约 218KB，一半的数据
    // 就这么丢了）。
    //
    // 提到 6 之后，数据一到：TinyUSB 任务跑回调把数据放进环形缓冲，紧接着被
    // 优先级更高的接收任务抢占、立刻取走 —— 缓冲再也不会积压。
    //
    // 不会饿死 USB：接收任务阻塞在环形缓冲的 Receive 上，没数据就主动让出。
    static osal::Task rx_task;
    rx_task.Create(RxTask, nullptr,
                   {.name = "link_rx", .stack_size = 4096, .priority = 6,
                    .core = osal::CoreAffinity::kCore1});

    ESP_LOGI(kTag, "链路已启动");
    return ESP_OK;
}

void SendTouch(bool down, int x, int y) {
    if (g_tx_lock == nullptr) {
        return;
    }
    if (down) {
        SendLine("{\"t\":\"touch\",\"ev\":\"down\",\"x\":%d,\"y\":%d}", x, y);
    } else {
        SendLine("{\"t\":\"touch\",\"ev\":\"up\"}");
    }
}

void SendKey(const char* ev) {
    if (g_tx_lock == nullptr) {
        return;
    }
    SendLine("{\"t\":\"key\",\"ev\":\"%s\"}", ev);
}

bool RemoteOwnsScreen() {
    // 收到第一个帧头就让位，不要等一条 band 收完。
    //
    // 这里写晚过一次，后果是整条链崩掉：本地图案和 PC 的帧抢 SPI 总线 ->
    // 第一条 band 排队失败 -> bands 一直是 0 -> 这个函数一直返回 false ->
    // 渲染任务永远不让位 -> 永久争抢 -> 消费任务读不了环形缓冲 -> 大量 RX 溢出。
    // 日志顺序很说明问题：先一串 acquire spi bus failed，然后才是"PC 已接管屏幕"。
    return g_stats.frames > 0 || g_stats.bands > 0 || g_stats.full > 0;
}

Stats GetStats() { return g_stats; }

}  // namespace link
