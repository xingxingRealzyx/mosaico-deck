// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT

#include "display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "fbox/board.h"
#include "fbox/config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "osal.h"

namespace display {
namespace {

constexpr const char* kTag = "display";
constexpr int kLcdHost = SPI2_HOST;

// 官方 Mosaico BSP 用 40 MHz。S31 的 GP-SPI 主机上限是 80 MHz，而 CO5300 是
// 只写设备（不需要 MISO 采样延时），所以 80 MHz 可行。
//
// 实测：60 MHz 时帧率 58 fps，80 MHz 时 83.5 fps，正好卡在
// 480*480*2 字节 / (80MHz * 4bit) = 11.5 ms -> 87 fps 的带宽上限上。
// 出现撕裂/花屏就把 menuconfig 里的 FBOX_DISPLAY_PCLK_MHZ 往下调
// —— 这个值**真的接到这里**了，不是摆设。
constexpr int kLcdPixelClockHz = CONFIG_FBOX_DISPLAY_PCLK_MHZ * 1000 * 1000;

// 两条 band 缓冲，DMA 可达的内部 SRAM。用 DMA_ATTR 保证不被放到 PSRAM。
DMA_ATTR uint16_t g_band_buf[2][fluidbox::config::kBandPixels];

// 面板没就绪时给渲染器用的哑缓冲。它会被正常写入，但 FlushBand 直接丢弃。
// 刻意**不加 DMA_ATTR**：它永远不会真的被 DMA 读，不该占用宝贵的内
// 部 DMA 可用 RAM。
uint16_t g_dummy_band[fluidbox::config::kBandPixels];

// TE 上升沿计数。只在 ISR 里加、在统计里读，所以 volatile 就够。
volatile uint32_t g_te_edges = 0;

// 每个 TE 上升沿放一个令牌，渲染任务靠它对齐面板刷新周期。
SemaphoreHandle_t g_te_sem = nullptr;

void IRAM_ATTR TeIsr(void* /*arg*/) {
    // C++20 起对 volatile 用 ++ 是弃用的（IDF 编译带 -Werror），显式赋值绕开。
    g_te_edges = g_te_edges + 1;
    if (g_te_sem != nullptr) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(g_te_sem, &woken);
        if (woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

osal::CountingSemaphore* g_buffers_free = nullptr;

bool IRAM_ATTR OnTransDone(esp_lcd_panel_io_handle_t io,
                           esp_lcd_panel_io_event_data_t* event,
                           void* user_ctx) {
    (void)io;
    (void)event;
    (void)user_ctx;
    if (g_buffers_free != nullptr) {
        g_buffers_free->Give();
    }
    return false;
}

// CO5300 上电时序：RGB565、亮度与 HBM 寄存器打开、设置全窗口、退出睡眠、开显示。
//
// 0x2A/0x2B 是 480x480 的全窗口（0x0000..0x01DF）。官方 BSP 特别提到
// board-manager 的默认表按 466x466 + 偏移 6 处理是错的，这里是修正过的版本。
// CO5300 初始化序列。
//
// **必须用这一份，不能用 esp_lcd_co5300 组件自带的默认表**，也不能自己凭直觉拼：
// 组件自带的是给另一块面板的（地址窗口 472x466），而 Mosaico 这块 480x480
// AMOLED 有几条不能少的命令：
//
//   0x11        睡眠退出，**必须是第一条**，而且要等 600 ms 让面板内部电源起来
//   0xFE=0x20   切到 page 0x20
//   0x19=0x10   面板内部振荡器/电源设置  ← 少了这条屏幕全程纯黑
//   0x1C=0xA0   同上                      ← 少了这条屏幕全程纯黑
//   0xFE=0x00   切回 page 0
//   0x2A/0x2B   地址窗口 0..479
//   0x29        开显示，再等 600 ms
//
// 这份序列取自 espressif/esp-brookesia 的 Mosaico 板级代码
// （hal/brookesia_hal_boards/components/mosaico/brookesia_hal_custom/src/board/display.c，
// 其注释说明改编自 esp-mosaico-bsp commit bef9967）。
// 踩过的坑：自己拼的那版有 0xFE/0xC4/0x3A/0x53/0x51/0x63/0x2A/0x2B/0x11/0x29，
// 唯独缺 0x19 和 0x1C，另外 0x11 放错位置、延迟给成了 100 ms —— 结果就是全程黑屏。
const co5300_lcd_init_cmd_t kInitCmds[] = {
    {0x11, nullptr, 0, 600},                                // sleep out，必须最先，600 ms
    {0xFE, (uint8_t[]){0x20}, 1, 0},                        // page 0x20
    {0x19, (uint8_t[]){0x10}, 1, 0},                        // 面板电源/振荡器
    {0x1C, (uint8_t[]){0xA0}, 1, 0},                        // 面板电源/振荡器
    {0xFE, (uint8_t[]){0x00}, 1, 0},                        // 回 page 0
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},                        // 16 bpp / RGB565
    // TE（撕裂同步）输出。官方 BSP 的 CONFIG_BSP_CO5300_ENABLE_TE 默认就是开的。
    // 除了防撕裂，它还给了我们一个**面板活着的硬件证据**：TE 引到 GPIO43，
    // 面板只要在扫描就会持续出脉冲，而 QSPI 本身是完全看不到反馈的。
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},                        // 亮度控制开
    {0x51, (uint8_t[]){0xFF}, 1, 0},                        // 满亮度
    {0x63, (uint8_t[]){0xFF}, 1, 0},                        // HBM 开
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},      // 列 0..479
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},      // 行 0..479
    {0x29, nullptr, 0, 600},                                // display on，600 ms
};

}  // namespace

Panel::~Panel() {
    if (buffers_free_ != nullptr) {
        delete static_cast<osal::CountingSemaphore*>(buffers_free_);
        buffers_free_ = nullptr;
    }
    g_buffers_free = nullptr;
}

esp_err_t Panel::Init() {
    // 屏幕供电：VCC_3V3_CTRL 低电平有效，上电默认是关的，不拉低面板毫无反应。
    // 这是这套硬件最容易踩的坑。
    gpio_config_t power_cfg = {};
    power_cfg.pin_bit_mask = 1ULL << fluidbox::board::kPinVcc3V3Ctrl;
    power_cfg.mode = GPIO_MODE_OUTPUT;
    power_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    power_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&power_cfg), kTag, "vcc ctrl gpio cfg");
    ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(fluidbox::board::kPinVcc3V3Ctrl), 0),
                        kTag, "vcc ctrl low");
    vTaskDelay(pdMS_TO_TICKS(20));  // 等 3.3V 轨稳定

    // 这里不检查 new 的返回值：CONFIG_COMPILER_CXX_EXCEPTIONS 没开，
    // 分配失败会直接 abort，检查分支永远不可达 —— 留着只会给人"已经处理过
    // OOM"的错觉。
    buffers_free_ = new osal::CountingSemaphore(2, 2);
    g_buffers_free = static_cast<osal::CountingSemaphore*>(buffers_free_);

    // 注意：**刻意不用 CO5300_PANEL_BUS_QSPI_CONFIG / CO5300_PANEL_IO_QSPI_CONFIG
    // 这两个宏**。它们是给 C 写的，展开后 (a) 字段顺序和结构体声明不一致，
    // C++ 的指定初始化器要求顺序严格一致，直接编不过；(b) 会把 int 赋给
    // gpio_num_t 枚举。逐个字段赋值既绕开这两个坑，也更好读。
    spi_bus_config_t bus_config = {};
    bus_config.sclk_io_num = fluidbox::board::Active().lcd_pclk;
    bus_config.data0_io_num = fluidbox::board::kPinLcdData0;
    bus_config.data1_io_num = fluidbox::board::kPinLcdData1;
    bus_config.data2_io_num = fluidbox::board::kPinLcdData2;
    bus_config.data3_io_num = fluidbox::board::kPinLcdData3;
    bus_config.max_transfer_sz = fluidbox::config::kBandPixels * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(static_cast<spi_host_device_t>(kLcdHost),
                                           &bus_config, SPI_DMA_CH_AUTO),
                        kTag, "spi bus init failed");

    // QSPI 的 5 根线要显式设驱动能力 —— 这不是可选项，官方 Mosaico 板级代码
    // 在创建面板之前专门做这一步（configure_display_drive_capability）。
    //
    // 官方默认给最弱挡（0），为的是降边沿速率和 EMI；但它的注释里也写明
    // "signal integrity fails at high clock 就往上调"。本工程默认给 2：
    // 60 MHz 下留出建立时间余量。**之前用最弱挡 + 80 MHz 时偶发位错误，
    // 表现是"某条 band 闪一下别的颜色"，而 QSPI 只写、主机侧完全看不出来。**
    {
        const gpio_num_t kDisplayPins[] = {
            static_cast<gpio_num_t>(fluidbox::board::Active().lcd_pclk),
            static_cast<gpio_num_t>(fluidbox::board::kPinLcdData0),
            static_cast<gpio_num_t>(fluidbox::board::kPinLcdData1),
            static_cast<gpio_num_t>(fluidbox::board::kPinLcdData2),
            static_cast<gpio_num_t>(fluidbox::board::kPinLcdData3),
        };
        for (gpio_num_t pin : kDisplayPins) {
            ESP_RETURN_ON_ERROR(gpio_set_drive_capability(
                                  pin, static_cast<gpio_drive_cap_t>(CONFIG_FBOX_LCD_DRIVE_CAP)),
                              kTag,
                                "lcd pin %d drive capability", static_cast<int>(pin));
        }
    }

    // QSPI 模式的参数和驱动默认值一致，只是命令/参数位宽是 32/8 且 pclk 我们
    // 自己定（默认 40 MHz，这里拉到 60）。
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = static_cast<gpio_num_t>(fluidbox::board::kPinLcdCs);
    io_config.dc_gpio_num = static_cast<gpio_num_t>(-1);  // QSPI 不需要 DC 线
    io_config.spi_mode = 0;
    io_config.pclk_hz = kLcdPixelClockHz;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = OnTransDone;
    io_config.user_ctx = nullptr;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = 1;

    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            static_cast<esp_lcd_spi_bus_handle_t>(kLcdHost),
                            &io_config, &io),
                        kTag, "panel io init failed");
    io_ = io;

    co5300_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = kInitCmds;
    vendor_config.init_cmds_size = sizeof(kInitCmds) / sizeof(kInitCmds[0]);
    vendor_config.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = static_cast<gpio_num_t>(fluidbox::board::Active().lcd_rst);
    panel_config.vendor_config = &vendor_config;

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_config, &panel), kTag,
                        "panel init failed");
    panel_ = panel;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), kTag, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), kTag, "panel setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), kTag,
                        "display on failed");

    // 把 TE 脚接成输入并计沿。**这不是为了防撕裂**（本工程不用），
    // 而是为了拿到"面板确实在扫描"这个硬件证据 —— QSPI 只写，没有它就只能靠眼睛。
    {
        gpio_config_t te = {};
        te.pin_bit_mask = 1ULL << fluidbox::board::kPinLcdTe;
        te.mode = GPIO_MODE_INPUT;
        te.pull_up_en = GPIO_PULLUP_DISABLE;
        te.pull_down_en = GPIO_PULLDOWN_DISABLE;
        te.intr_type = GPIO_INTR_POSEDGE;
        if (g_te_sem == nullptr) {
            g_te_sem = xSemaphoreCreateBinary();
        }
        if (gpio_config(&te) == ESP_OK) {
            // 重复安装会返回 ESP_ERR_INVALID_STATE，忽略即可。
            (void)gpio_install_isr_service(0);
            (void)gpio_isr_handler_add(static_cast<gpio_num_t>(fluidbox::board::kPinLcdTe),
                                       TeIsr, nullptr);
            ESP_LOGI(kTag, "TE 计数已启用 (GPIO%d)", fluidbox::board::kPinLcdTe);
        }
    }

    ESP_LOGI(kTag, "CO5300 %dx%d @ %d MHz 就绪 (board %s)", fluidbox::config::kLcdHRes,
             fluidbox::config::kLcdVRes, kLcdPixelClockHz / 1000000, fluidbox::board::Active().name);
    return ESP_OK;
}

esp_err_t Panel::SetBrightness(uint8_t level) {
    if (panel_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // 走驱动提供的 API，而不是自己往 0x51 写一个原始字节：
    // 它做参数范围校验、把百分比换算成硬件值、并且会打一条日志说明实际写进去
    // 的是什么，排查"屏幕太暗/太亮"时这条日志有用。
    //
    // 我们的接口是 0..255（和寄存器同量纲，调参时更直观），驱动要的是 0..100，
    // 所以在这里换算。
    const unsigned percent =
        (static_cast<unsigned>(level) * 100u + 127u) / 255u;  // 四舍五入
    return esp_lcd_panel_co5300_set_brightness(
        static_cast<esp_lcd_panel_handle_t>(panel_),
        static_cast<uint8_t>(percent > 100u ? 100u : percent));
}

uint16_t* Panel::AcquireBand() {
    auto* sem = static_cast<osal::CountingSemaphore*>(buffers_free_);
    if (sem == nullptr) {
        // 面板没就绪（FBOX_SKIP_PANEL，或 Init 中途失败）。
        // 这里绝不能解引用空指针 —— 那会把"没有面板"变成"系统崩溃重启"。
        return g_dummy_band;
    }
    if (!sem->Take(kAcquireTimeoutMs)) {
        // 传输完成中断没回来（最可能是 FlushBand 排队失败过）。
        // 丢这一帧并留下证据，绝不无限等下去 —— 无限等是静默冻屏，
        // 比丢帧难查得多。
        ESP_LOGE(kTag, "band 缓冲等超时（传输完成中断没回来？），本帧丢弃");
        return g_dummy_band;
    }
    return g_band_buf[next_buffer_++ & 1u];
}

esp_err_t Panel::FlushBand(int band_index, const uint16_t* buffer) {
    if (panel_ == nullptr) {
        // 面板没就绪：内容丢掉即可，这不是错误。
        return ESP_ERR_INVALID_STATE;
    }
    const int y0 = band_index * fluidbox::config::kBandRows;
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        static_cast<esp_lcd_panel_handle_t>(panel_), 0, y0,
        fluidbox::config::kLcdHRes, y0 + fluidbox::config::kBandRows, buffer);
    if (err != ESP_OK) {
        // 排队失败 -> 完成中断永远不会触发 -> 这块缓冲再也回不来。
        // 必须在这里还回去，否则连丢两次之后渲染任务就会一直等不到缓冲。
        auto* sem = static_cast<osal::CountingSemaphore*>(buffers_free_);
        if (sem != nullptr) {
            sem->Give();
        }
        ESP_LOGE(kTag, "band %d 排队失败: %s（缓冲已归还）", band_index,
                 esp_err_to_name(err));
    }
    return err;
}

uint32_t Panel::TeEdges() const { return g_te_edges; }

bool Panel::WaitForTeTick(uint32_t timeout_ms) {
    if (g_te_sem == nullptr) {
        return false;
    }
    // 先把可能积压的旧令牌清掉，确保等到的是"下一个"沿。
    (void)xSemaphoreTake(g_te_sem, 0);
    return xSemaphoreTake(g_te_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void Panel::PrepareForBootloader() {
    if (panel_ == nullptr) {
        return;
    }
    // 先关显示再等等 —— 只写总线，没法确认，只能按最坏情况留时间。
    (void)esp_lcd_panel_disp_on_off(static_cast<esp_lcd_panel_handle_t>(panel_), false);
    (void)SetBrightness(0);
    vTaskDelay(pdMS_TO_TICKS(50));
}

}  // namespace display
