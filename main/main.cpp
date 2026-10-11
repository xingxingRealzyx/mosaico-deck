// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// mosaico-deck —— 桌面的 agent 状态/交互终端
//
// 架构（为什么界面不在这块板子上渲染）
// ----------------------------------
// 板子只做三件事：**显示**、**采集输入**、**跟 PC 通信**。
// 界面（文字、图标、布局、颜色）全部在 PC 侧渲染好，按 band 发过来。
//
// 理由不是"板子性能不够"，而是**开发效率**：
//   · 中文要自己做点阵字库（1~2MB，而且难看）；PC 上有系统字体，完美且免费
//   · 抗锯齿、图标、emoji —— PC 上全免费
//   · 改一次界面：PC 侧存盘即生效，板子侧要重新烧录
// 最后一条是数量级的差别。板子侧代码越少，"调界面"这件事就越快。
//
// 而且板子是通过 USB 连在电脑上的，带宽和供电都不成问题 —— 450KB 一帧整屏传也就
// 0.5 秒；只传变化的 band 的话是几 KB、几十毫秒。

#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fbox/board.h"
#include "fbox/config.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio.h"
#include "dh_logo.h"
#include "input.h"
#include "hostlink.h"
#include "osal.h"
#include "touch.h"
#include "usb_console.h"

namespace {

constexpr const char* kTag = "deck";

display::Panel g_panel;
fluidbox::touch::Touchscreen g_touch;
input::ResetButton g_button;
i2c_master_bus_handle_t g_i2c_bus = nullptr;

uint32_t g_frames = 0;
uint32_t g_band_flushes = 0;

// ---------------------------------------------------------------------------
// 电源：屏幕的 3.3V 控制脚
// ---------------------------------------------------------------------------
// GPIO60 控制屏幕供电及对外 3.3V 输出，**低电平有效**。上电时它是关的，
// 不主动拉低面板就完全没反应 —— 而且 QSPI 是只写总线，主机侧一点都看不出来。
esp_err_t PowerOnRails() {
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << fluidbox::board::kPinVcc3V3Ctrl,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), kTag, "配置 GPIO%d 失败",
                        fluidbox::board::kPinVcc3V3Ctrl);
    // 先给电平再使能输出，避免上电瞬间出现毛刺。
    ESP_RETURN_ON_ERROR(
        gpio_set_level(static_cast<gpio_num_t>(fluidbox::board::kPinVcc3V3Ctrl), 0), kTag,
                        "拉低 VCC_3V3 失败");
    // 电源爬升需要一点时间。官方用 LEDC 做斜坡，这里直接等 —— 差别只在
    // 上电瞬间的浪涌，对功能没有影响。
    osal::Delay(120);
    ESP_LOGI(kTag, "屏幕 3.3V 已使能 (GPIO%d=0)",
             fluidbox::board::kPinVcc3V3Ctrl);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2C 引脚排查
// ---------------------------------------------------------------------------
// **文档写的和实测不一样。** 官方 V1.0 文档说对内 I2C0 在 GPIO0/1，但实测这块
// 板子上一个器件都扫不到；GPIO56/3 上却有 6 个（触摸、IMU、磁力计、电量计、
// codec）。所以启动时两组都扫一遍，选用有器件的那组。
//
// 顺带这是判定板子版本的**第二路独立信号**：V1.0 的 I2C0 在 GPIO0/1，
// V1.2 在 GPIO56/3。
int ScanBusOnPins(int sda, int scl, uint32_t hz) {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_1;  // 用 1 号口扫描，扫完就释放，不影响正事
    cfg.sda_io_num = static_cast<gpio_num_t>(sda);
    cfg.scl_io_num = static_cast<gpio_num_t>(scl);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = 1;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        return 0;
    }

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(kTag, "  I2C(%d,%d) 发现器件 0x%02X", sda, scl, addr);
            found++;
        }
    }
    i2c_del_master_bus(bus);
    return found;
}

// ---------------------------------------------------------------------------
// 显示链路自检
// ---------------------------------------------------------------------------
// 这一版还没有 PC 送帧，所以自己画一个图案证明整条链路通：**16 条 band 是不是
// 都在动、颜色字节序对不对、有没有哪条 band 被漏掉**。
//
// 图案：整屏竖向渐变（每条 band 的底色不同 → 漏掉一条会看出一条硬边），
// 加一根横向移动的亮条（证明在持续刷新），四角各一个白方块（证明边界覆盖）。
// ---------------------------------------------------------------------------
// 显示链路自检
// ---------------------------------------------------------------------------
// 关键点：**只重画变化的那几条 band，不整屏重推。**
//
// 为什么这是重点：整屏 450KB 在 60MHz QSPI 上要 15.4ms，而面板刷新周期是 17ms
// —— 一帧的传输时间几乎等于一次刷新，只要有一点抖动就会跨过去，上半屏是新内容、
// 下半屏还是旧的，看起来就是撕裂（实测一根横扫的亮条"有时候会弯"）。
//
// 而真实 UI 大部分时间是静态的，只有几条 band 会变：文字更新也就 1~3 条，
// 1.4~4.2ms 就传完了，稳稳落在一次刷新之内，**根本不会撕裂**。
//
// 所以正确的做法不是"让动画慢下来"（那只是把问题藏起来），而是只发变化的部分。
//
// ---------------------------------------------------------------------------
// 开机动画：DeepSeek Harness 的鲸鱼 logo -> 碎成粒子散开 -> 交给 PC
// ---------------------------------------------------------------------------
// **动画必须在板子上跑。** PC 推帧的话一帧是 16 条 band / 450KB，光传输就要
// 15.4ms，加上渲染往返根本做不出流畅效果；而板子本地画只受 QSPI 带宽限制，
// 实测能到 ~40fps，足够了。
//
// 三个阶段：
//   ① 浮现（0.0~0.7s）  logo 由暗到亮淡入
//   ② 散开（0.7~2.2s）  logo 的像素变成粒子，向外飞散并淡出
//   ③ 交棒（2.2s 之后）  黑屏等待 —— PC 一旦送来第一帧就永久让位
//
// 粒子位置用解析式算（p = p0 + v*t）而不是逐帧累加：一来不会有浮点漂移，
// 二来任意时刻都能直接算出来，不受掉帧影响。
namespace boot {

using fluidbox::config::kBandCount;
using fluidbox::config::kBandPixels;
using fluidbox::config::kBandRows;
using fluidbox::config::kLcdHRes;
using fluidbox::config::kLcdVRes;

constexpr int64_t kFadeInUs = 600 * 1000;    // ① 浮现
constexpr int64_t kScatterUs = 1100 * 1000;  // ② 散开
// ③ 余韵：不再"结束"，而是落进一个安静的状态**一直陪着**，直到 PC 送第一帧。
//
// 这里踩过一次：原来动画在 2.2s 硬结束然后黑屏等 PC。但粒子带各自的起飞延迟，
// 最晚的 ~1.7s 就淡完了，于是最后 0.5s 是纯黑；再加上 PC 侧建立连接的 1.2s，
// 用户看到的是"炸开之后黑了好一会儿"。
//
// 现在动画不存在"结束"这个状态 —— 屏幕永远有东西，PC 一来就自然接走。
constexpr int64_t kScatterEndUs = kFadeInUs + kScatterUs;

// logo 居中
constexpr int kLogoX = (kLcdHRes - deck::kLogoSize) / 2;
constexpr int kLogoY = (kLcdVRes - deck::kLogoSize) / 2;

// 采样间隔：蒙版里每隔 kSample 个像素取一个粒子。
// 3 -> 约 1100 个粒子，散开时足够密，又不至于每帧算不过来。
constexpr int kSample = 3;
constexpr int kMaxParticles = (deck::kLogoSize / kSample) * (deck::kLogoSize / kSample);

struct Particle {
    float x0, y0;   // 起点（相对屏幕中心）
    float vx, vy;   // 速度（像素/秒）
    float delay;    // 各自的起飞延迟，让散开有层次而不是"整块平移"
};

Particle g_particles[kMaxParticles];
int g_count = 0;

// 一个确定性的伪随机 —— 不用 rand() 是因为它要 srand 且结果依赖 libc 版本，
// 换个工具链开机动画就变了。这里用整数 hash，结果永远一样。
inline uint32_t Hash(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

void InitParticles() {
    if (g_count > 0) {
        return;
    }
    const int half = deck::kLogoSize / 2;
    for (int y = 0; y < deck::kLogoSize; y += kSample) {
        for (int x = 0; x < deck::kLogoSize; x += kSample) {
            const uint8_t byte = deck::kLogoMask[y * deck::kLogoStride + (x >> 3)];
            if ((byte & (0x80 >> (x & 7))) == 0) {
                continue;  // 不属于 logo
            }
            if (g_count >= kMaxParticles) {
                break;
            }
            const float dx = static_cast<float>(x - half);
            const float dy = static_cast<float>(y - half);
            // 离中心越远飞得越快 —— 视觉上就是"从中间炸开"
            const float dist = sqrtf(dx * dx + dy * dy) + 1.0f;
            const uint32_t h = Hash(static_cast<uint32_t>(y * 1000 + x));
            const float jitter = 0.75f + static_cast<float>(h % 1000) / 1000.0f * 0.7f;
            const float speed = (90.0f + dist * 1.9f) * jitter;

            Particle& p = g_particles[g_count++];
            p.x0 = dx;
            p.y0 = dy;
            p.vx = dx / dist * speed;
            p.vy = dy / dist * speed;
            // 延迟 0~180ms，按 hash 散开
            p.delay = static_cast<float>((h >> 10) % 180) / 1000.0f;
        }
    }
    ESP_LOGI(kTag, "开机动画：%d 个粒子", g_count);
}

// 把某条 band 的一行像素填成黑（RGB565 且高字节在前，和 PC 侧一致）
inline uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return static_cast<uint16_t>((v << 8) | (v >> 8));  // 字节交换
}

const uint16_t kBlack = Rgb565(0, 0, 0);

// 画一帧。full_redraw 时 16 条全画（第一帧初始化整屏）。
//
// **这个函数没有"结束"**：elapsed_us 一直涨，画出来的东西自然从"散开"过渡到
// "余韵"，永远有内容。PC 接管由渲染循环判断，不在这里。
void DrawFrame(int64_t elapsed_us, bool full_redraw) {
    InitParticles();
    const bool scattering = elapsed_us >= kFadeInUs;
    const float st = static_cast<float>(elapsed_us - kFadeInUs) / 1e6f;
    // 散开结束后的秒数（余韵阶段用）
    const float after = (elapsed_us > kScatterEndUs)
                            ? static_cast<float>(elapsed_us - kScatterEndUs) / 1e6f
                            : 0.0f;

    for (int band = 0; band < kBandCount; band++) {
        const int y0 = band * kBandRows;
        uint16_t* buf = g_panel.AcquireBand();
        if (buf == nullptr) {
            return;  // 超时，这一帧就放弃（下一帧再来）
        }
        for (int i = 0; i < kBandPixels; i++) {
            buf[i] = kBlack;
        }

        if (!scattering) {
            // ---- 阶段①：logo 淡入 ----
            const float t = static_cast<float>(elapsed_us) / static_cast<float>(kFadeInUs);
            const uint8_t level = static_cast<uint8_t>(t * 255.0f);
            const uint16_t color = Rgb565(level, level, level);

            for (int y = 0; y < deck::kLogoSize; y++) {
                const int sy = kLogoY + y;
                if (sy < y0 || sy >= y0 + kBandRows) {
                    continue;
                }
                uint16_t* row = buf + (sy - y0) * kLcdHRes;
                const uint8_t* mask_row = &deck::kLogoMask[y * deck::kLogoStride];
                for (int x = 0; x < deck::kLogoSize; x++) {
                    if ((mask_row[x >> 3] & (0x80 >> (x & 7))) != 0) {
                        row[kLogoX + x] = color;
                    }
                }
            }
        } else {
            // ---- 阶段②③：粒子散开 -> 余韵 ----
            // 每个粒子画 2×2，保证远看还是"有点东西"而不是稀疏的点。
            //
            // 亮度衰减用的是 (1 - local/1.5)^2，所以最晚的粒子（delay 最大 0.18s）
            // 在 st≈1.68s 就彻底暗了 —— 而散开阶段是 1.1s，也就是说**粒子还没飞完
            // 就已经看不见了**。这正是"炸开之后黑一段"的一半原因。
            // 现在 fade 的分母拉到和整个散开时长一致，并且余韵阶段保留一点余光。
            for (int i = 0; i < g_count; i++) {
                const Particle& p = g_particles[i];
                const float local = st - p.delay;
                if (local <= 0.0f) {
                    continue;
                }
                float fade = 1.0f - local / 1.35f;
                if (fade <= 0.0f) {
                    // 余韵：只剩极少数粒子还在缓慢外飘，给屏幕留一点"呼吸"
                    // 用 (i % 23) 挑，比例约 4%，看起来是零星的星点而不是噪点
                    if ((i % 23) != 0) {
                        continue;
                    }
                    fade = 0.16f + 0.10f * sinf(after * 1.6f + static_cast<float>(i));
                    if (fade <= 0.0f) {
                        continue;
                    }
                    const float slow = local * 0.35f;  // 余韵阶段几乎停住
                    const float px0 = kLcdHRes * 0.5f + p.x0 + p.vx * slow;
                    const float py0 = kLcdVRes * 0.5f + p.y0 + p.vy * slow;
                    const int ix0 = static_cast<int>(px0);
                    const int iy0 = static_cast<int>(py0);
                    if (iy0 >= y0 && iy0 < y0 + kBandRows && ix0 >= 0 && ix0 < kLcdHRes - 1) {
                        const uint8_t lv = static_cast<uint8_t>(fade * 255.0f);
                        const uint16_t c = Rgb565(lv, lv, lv);
                        uint16_t* r0 = buf + (iy0 - y0) * kLcdHRes;
                        r0[ix0] = c;
                        r0[ix0 + 1] = c;
                    }
                    continue;
                }
                // 散开阶段：亮度按平方衰减，尾巴收得干脆
                fade = fade * fade;
                const float px = kLcdHRes * 0.5f + p.x0 + p.vx * local;
                const float py = kLcdVRes * 0.5f + p.y0 + p.vy * local;
                const int ix = static_cast<int>(px);
                const int iy = static_cast<int>(py);
                if (iy < y0 || iy >= y0 + kBandRows) {
                    continue;
                }
                if (ix < 0 || ix >= kLcdHRes - 1) {
                    continue;
                }
                const uint8_t level = static_cast<uint8_t>(fade * 255.0f);
                const uint16_t color = Rgb565(level, level, level);
                uint16_t* row = buf + (iy - y0) * kLcdHRes;
                row[ix] = color;
                row[ix + 1] = color;
                if (iy + 1 < y0 + kBandRows) {
                    uint16_t* row2 = row + kLcdHRes;
                    row2[ix] = color;
                    row2[ix + 1] = color;
                }
            }
        }

        if (g_panel.FlushBand(band, buf) == ESP_OK) {
            g_band_flushes = g_band_flushes + 1;
        }
    }
}

}  // namespace boot

// ---------------------------------------------------------------------------
// 任务
// ---------------------------------------------------------------------------
void RenderTask(void* arg) {
    (void)arg;
    ESP_LOGI(kTag, "渲染任务启动");

    // 本地自检图案只在**PC 还没接管屏幕**时画。
    //
    // 为什么必须让出去：两个任务同时画会抢 SPI 总线（实测 `acquire spi bus failed`
    // + `ESP_ERR_INVALID_STATE`），而且消费任务被总线卡住之后没空读环形缓冲 ——
    // 表现为大量 RX 溢出。屏幕的所有权只能有一个主人。
    const int64_t boot_start_us = osal::TimeUs();
    bool first = true;
    bool gave_up = false;
    bool anim_done = false;
    for (;;) {
        if (hostlink::RemoteOwnsScreen()) {
            if (!gave_up) {
                gave_up = true;
                ESP_LOGI(kTag, "PC 已接管屏幕，开机动画让位");
            }
            osal::Delay(50);
            continue;
        }

        // **动画没有"结束"，一直画到 PC 接管为止。**
        //
        // 原来是在 2.2s 硬停下来黑屏等 PC，结果"炸开之后黑了好一会儿"——
        // 因为粒子带延迟，最晚的 1.7s 就淡完了，剩下的时间是纯黑；再叠上 PC
        // 建立连接的时间就更长。现在动画落进一个安静的余韵状态陪着屏幕，
        // PC 一来自然被接走。
        //
        // 唯一要守的规矩：**PC 一接管就必须立刻停手** —— 两边同时画会抢 SPI
        // 总线（实测 acquire spi bus failed），而且消费任务被卡住会导致 RX 溢出。
        // 这个由循环开头的 RemoteOwnsScreen() 保证。
        const int64_t elapsed = osal::TimeUs() - boot_start_us;
        if (!anim_done && elapsed > boot::kScatterEndUs + 1500 * 1000) {
            anim_done = true;
            ESP_LOGI(kTag, "开机动画进入余韵（%.1fs）", elapsed / 1e6);
        }

        // 等面板的一个刷新周期再推帧。面板刷新率（约 58.5Hz）才是这个系统的真实
        // 上限，推得比它快不会更流畅，只会在面板扫描时往 GRAM 里写 —— 那样整条
        // band 可能整帧都没被接受。
        if (g_panel.WaitForTeTick(30)) {
            // 和 link 抢的是同一把锁 —— 见 display::Panel::DrawMutex()。
            osal::LockGuard<osal::Mutex> draw_guard(g_panel.DrawMutex());
            boot::DrawFrame(elapsed, first);
            first = false;
            g_frames = g_frames + 1;
        }
    }
}

// 触摸上报。**只在按下/抬起边沿发** —— 触摸是 3ms 轮询一次，逐个采样都发的话
// 每秒 300 条消息，USB 会被刷爆，而 PC 侧真正需要的只是"手指落在哪、什么时候抬"。
void TouchTask(void* arg) {
    (void)arg;
    bool was_active = false;
    bool rec_held = false;
    int64_t last_button_us = osal::TimeUs();
    for (;;) {
        g_touch.Poll();
        const fluidbox::touch::State ts = g_touch.Get();
        if (ts.active != was_active) {
            was_active = ts.active;
            hostlink::SendTouch(ts.active, ts.x, ts.y);
            ESP_LOGI(kTag, "触摸 %s (%d,%d)", ts.active ? "按下" : "抬起", ts.x, ts.y);
        }

        // 按键也在这里轮询 —— 单独起一个任务只为了每 25ms 读一次 GPIO 不值得。
        //
        // 按键的两个用途：
        //   · **按住 = 说话**（最自然的交互：想说就说，不用在屏幕上找按钮）
        //   · 单击 = 取消当前的问询（PC 侧处理）
        const int64_t now = osal::TimeUs();
        if (now - last_button_us >= 25 * 1000) {
            last_button_us = now;

            // 先看电平：按住期间持续录音。
            const bool held = g_button.IsPressed();
            if (held != rec_held) {
                rec_held = held;
                if (held) {
                    hostlink::StartCapture();
                    ESP_LOGI(kTag, "按键按下 -> 开始录音");
                } else {
                    hostlink::StopCapture();
                    ESP_LOGI(kTag, "按键松开 -> 停止录音");
                    hostlink::SendKey("voice_end");
                }
                // 录音期间的按下/松开不再当作短按长按上报，避免 PC 侧收到
                // 一个"短按"然后又收到一段语音，语义打架。
                (void)g_button.Poll();
            } else {
                switch (g_button.Poll()) {
                    case input::ResetButton::Event::kShortPress:
                        hostlink::SendKey("short");
                        ESP_LOGI(kTag, "按键：短按");
                        break;
                    case input::ResetButton::Event::kLongPress:
                        hostlink::SendKey("long");
                        ESP_LOGI(kTag, "按键：长按");
                        break;
                    default:
                        break;
                }
            }
        }

        osal::Delay(fluidbox::config::kTouchPollPeriodMs);
    }
}

void StatsTask(void* arg) {
    (void)arg;
    int64_t last_us = osal::TimeUs();
    uint32_t last_frames = 0;
    uint32_t last_flushes = 0;

    for (;;) {
        osal::Delay(fluidbox::config::kStatsPeriodMs);

        const int64_t now_us = osal::TimeUs();
        const float dt = static_cast<float>(now_us - last_us) * 1e-6f;
        last_us = now_us;

        const uint32_t df = g_frames - last_frames;
        const uint32_t db = g_band_flushes - last_flushes;
        last_frames = g_frames;
        last_flushes = g_band_flushes;

        const fluidbox::touch::State ts = g_touch.Get();
        // df/dt 和 db/dt —— **两个数都要除以 dt**。
        // 之前 db 直接打出来，于是 1872 这个"每 2 秒的次数"看着像频率，
        // 和 58.5 fps 对不上（1872/58.5 不是整数）。除以 dt 之后是 936 band/s
        // = 58.5 fps × 16 条，才对得上。
        const hostlink::Stats ls = hostlink::GetStats();
        ESP_LOGI(kTag, "音频 放音%u 录音%u 块 | %s",
                 static_cast<unsigned>(ls.audio_in), static_cast<unsigned>(ls.audio_out),
                 hostlink::Capturing() ? "正在录音" : "空闲");
        ESP_LOGI(kTag, "链路 帧%u band%u 整屏%u 控制%u | 坏帧头%u 坏长度%u 超时%u | RX溢出%u",
                 static_cast<unsigned>(ls.frames), static_cast<unsigned>(ls.bands),
                 static_cast<unsigned>(ls.full), static_cast<unsigned>(ls.ctrl),
                 static_cast<unsigned>(ls.bad_magic), static_cast<unsigned>(ls.bad_len),
                 static_cast<unsigned>(ls.rx_timeout),
                 static_cast<unsigned>(usb_console::RxOverflowCount()));
        ESP_LOGI(kTag, "%.1f fps | %.0f band/s | TE %u | 触摸 %s(%d,%d) | SRAM %u B",
                 static_cast<double>(df) / dt, static_cast<double>(db) / dt,
                 static_cast<unsigned>(g_panel.TeEdges()),
                 ts.active ? "按下" : "抬起", ts.x, ts.y,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    }
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "===== mosaico-deck 启动 =====");

    // ---- 板级版本：必须最先做 ----
    // 读 eFuse 决定 LCD_RST/LCD_SCL 和 I2C 引脚。V1.0 和 V1.2 这两组是互换的，
    // 猜错的后果是"面板初始化成功但全程黑屏"（QSPI 只写，主机不知情）。
    fluidbox::board::Init();

    // ---- USB 控制台 ----
    // 放前面：后面所有日志都要靠它出去。注意 USB 枚举要几百毫秒，
    // 这之前的日志会丢（官方 SDK 也一样）。
    if (usb_console::Init() != ESP_OK) {
        ESP_LOGW(kTag, "USB 控制台初始化失败");
    }
    usb_console::AttachToLogger();

    // ---- 屏幕供电 ----
    if (PowerOnRails() != ESP_OK) {
        ESP_LOGE(kTag, "3.3V 上电失败");
    }

    // ---- I2C 引脚排查 ----
    const int n_v10 = ScanBusOnPins(0, 1, 100000);
    const int n_v12 = ScanBusOnPins(56, 3, 100000);
    // 先报给 board：eFuse 不可信时这是判定板子版本的第二路独立信号。
    fluidbox::board::SetRevisionFromI2cProbe(n_v12 > n_v10);
    ESP_LOGI(kTag, "I2C 探测: V1.0 组 %d 个 / V1.2 组 %d 个", n_v10, n_v12);

    const int sda = fluidbox::board::Active().i2c_sda;
    const int scl = fluidbox::board::Active().i2c_scl;

    i2c_master_bus_config_t i2c_cfg = {};
    i2c_cfg.i2c_port = fluidbox::board::kI2cPort;
    i2c_cfg.sda_io_num = static_cast<gpio_num_t>(sda);
    i2c_cfg.scl_io_num = static_cast<gpio_num_t>(scl);
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&i2c_cfg, &g_i2c_bus) != ESP_OK) {
        ESP_LOGE(kTag, "I2C 总线创建失败 (SDA=%d SCL=%d)", sda, scl);
        g_i2c_bus = nullptr;
    }

    // ---- 面板 ----
    if (g_panel.Init() != ESP_OK) {
        ESP_LOGE(kTag, "面板初始化失败");
    } else {
        g_panel.SetBrightness(fluidbox::config::kDisplayBrightness);
        ESP_LOGI(kTag, "面板就绪，亮度 %d", fluidbox::config::kDisplayBrightness);
    }

    // ---- 触摸 ----
    if (g_i2c_bus != nullptr) {
        if (g_touch.Init(g_i2c_bus) != ESP_OK) {
            ESP_LOGW(kTag, "触摸初始化失败，不影响显示");
        }
    }

    // ---- 按键 ----
    if (g_button.Init() != ESP_OK) {
        ESP_LOGW(kTag, "按键初始化失败");
    }

    // 进 ROM 下载模式之前先关屏：重启后 GPIO 状态会丢、ROM 也不驱动屏幕的 3.3V
    // 控制脚，屏幕要是还亮着，烧 flash 的电流尖峰就可能把供电拉崩。
    usb_console::SetPreRestartHook([]() { g_panel.PrepareForBootloader(); });

    // ---- 音频 ----
    // ES8311 挂在 I2C 上，所以必须在 I2C 总线建好之后。
    // 失败了不影响显示和触摸，只是不能说话/听。
    if (g_i2c_bus != nullptr) {
        if (audio::Init(g_i2c_bus) != ESP_OK) {
            ESP_LOGW(kTag, "音频初始化失败，设备不能说话也不能听");
        }
    }

    // ---- 链路 ----
    // SetPanel 必须在 Init 之前 —— link 不认识面板从哪来，由 main 决定。
    // 面板没就绪时传 nullptr 也合法：link 照常收帧、只是不显示（否则流会错位）。
    hostlink::SetPanel(g_panel.ready() ? &g_panel : nullptr);
    hostlink::Init();

    // ---- 起任务 ----
    static osal::Task render_task;
    static osal::Task stats_task;
    static osal::Task touch_task;
    render_task.Create(RenderTask, nullptr,
                       {.name = "render", .stack_size = 4096, .priority = 5,
                        .core = osal::CoreAffinity::kCore0});
    touch_task.Create(TouchTask, nullptr,
                      {.name = "touch", .stack_size = 3072, .priority = 4,
                       .core = osal::CoreAffinity::kCore1});
    stats_task.Create(StatsTask, nullptr,
                      {.name = "stats", .stack_size = 3072, .priority = 3,
                       .core = osal::CoreAffinity::kCore1});

    ESP_LOGI(kTag, "启动完成。现在等 PC 送帧；本地自检图案：渐变 + 横扫亮条 + 四角块");
}
