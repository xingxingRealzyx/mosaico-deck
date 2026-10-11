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
#include "input.h"
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
void DrawBringUpPattern(int frame) {
    for (int band = 0; band < fluidbox::config::kBandCount; band++) {
        uint16_t* buf = g_panel.AcquireBand();
        if (buf == nullptr) {
            continue;
        }

        // 底色：按 band 序号在深蓝→深紫之间渐变。RGB565 要先转字节序，
        // 面板要的字节序和 CPU 存 uint16_t 的相反。
        const int t = band * 255 / (fluidbox::config::kBandCount - 1);
        auto swap16 = [](uint16_t v) -> uint16_t { return static_cast<uint16_t>((v >> 8) | (v << 8)); };
        const uint16_t r5 = static_cast<uint16_t>(t * 31 / 255);
        const uint16_t b5 = static_cast<uint16_t>((255 - t) * 31 / 255);
        const uint16_t bg = swap16(static_cast<uint16_t>((r5 << 11) | b5));

        for (int i = 0; i < fluidbox::config::kBandPixels; i++) {
            buf[i] = bg;
        }

        // 横向移动的亮条：周期 240 帧，来回扫。
        const int phase = frame % 240;
        const int x = (phase < 120) ? (phase * 4) : ((239 - phase) * 4);
        const int y0 = band * fluidbox::config::kBandRows;
        const int y1 = y0 + fluidbox::config::kBandRows;
        if (y1 > 200 && y0 < 280) {
            const uint16_t bright = swap16(0xFFFF);
            for (int y = (y0 > 200 ? y0 : 200); y < (y1 < 280 ? y1 : 280); y++) {
                uint16_t* row = buf + (y - y0) * fluidbox::config::kLcdHRes;
                for (int dx = 0; dx < 12; dx++) {
                    const int px = x + dx;
                    if (px >= 0 && px < fluidbox::config::kLcdHRes) {
                        row[px] = bright;
                    }
                }
            }
        }

        // 四角的白方块：证明屏幕四边都被真正覆盖到。
        const uint16_t white = swap16(0xFFFF);
        const int corners[4][2] = {{0, 0}, {fluidbox::config::kLcdHRes - 20, 0},
                                   {0, fluidbox::config::kLcdVRes - 20},
                                   {fluidbox::config::kLcdHRes - 20, fluidbox::config::kLcdVRes - 20}};
        for (auto& c : corners) {
            for (int y = c[1]; y < c[1] + 20; y++) {
                if (y < y0 || y >= y1) {
                    continue;
                }
                uint16_t* row = buf + (y - y0) * fluidbox::config::kLcdHRes;
                for (int x = c[0]; x < c[0] + 20; x++) {
                    if (x >= 0 && x < fluidbox::config::kLcdHRes) {
                        row[x] = white;
                    }
                }
            }
        }

        if (g_panel.FlushBand(band, buf) == ESP_OK) {
            g_band_flushes = g_band_flushes + 1;
        }
    }
}

// ---------------------------------------------------------------------------
// 任务
// ---------------------------------------------------------------------------
void RenderTask(void* arg) {
    (void)arg;
    ESP_LOGI(kTag, "渲染任务启动");

    int frame = 0;
    for (;;) {
        // 等面板的一个刷新周期再推帧。面板刷新率（约 58.5Hz）才是这个系统的真实
        // 上限，推得比它快不会更流畅，只会在面板扫描时往 GRAM 里写 —— 那样整条
        // band 可能整帧都没被接受。
        g_panel.WaitForTeTick(30);

        DrawBringUpPattern(frame);
        frame++;
        g_frames = g_frames + 1;
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
        ESP_LOGI(kTag, "%.1f fps | %u band/s | TE %u | 触摸 %s(%d,%d) | SRAM %u B",
                 static_cast<double>(df) / dt, static_cast<unsigned>(db),
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

    // ---- 起任务 ----
    static osal::Task render_task;
    static osal::Task stats_task;
    render_task.Create(RenderTask, nullptr,
                       {.name = "render", .stack_size = 4096, .priority = 5,
                        .core = osal::CoreAffinity::kCore0});
    stats_task.Create(StatsTask, nullptr,
                      {.name = "stats", .stack_size = 3072, .priority = 3,
                       .core = osal::CoreAffinity::kCore1});

    ESP_LOGI(kTag, "启动完成，期待看到：整屏竖向渐变 + 一根横扫的亮条 + 四角白块");
}
