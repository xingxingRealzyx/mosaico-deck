// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT

#include "display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
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

// The official Mosaico BSP uses 40 MHz. The GP-SPI host on the S31 tops out at
// 80 MHz, and the CO5300 is a write-only device (no MISO sampling delay needed),
// so 80 MHz is feasible.
//
// Measured: 58 fps at 60 MHz and 83.5 fps at 80 MHz, right against the bandwidth
// limit of 480*480*2 bytes / (80MHz * 4bit) = 11.5 ms -> 87 fps.
// If tearing or corruption shows up, lower FBOX_DISPLAY_PCLK_MHZ in menuconfig
// -- this value is **genuinely wired in here**, it is not decorative.
constexpr int kLcdPixelClockHz = CONFIG_FBOX_DISPLAY_PCLK_MHZ * 1000 * 1000;

// Two band buffers in internal SRAM reachable by DMA. DMA_ATTR keeps them out of
// PSRAM.
DMA_ATTR uint16_t g_band_buf[2][fluidbox::config::kBandPixels];

// Dummy buffer for the renderer when the panel is not ready. It is written
// normally, but FlushBand simply drops it. Deliberately **not** marked with
// DMA_ATTR: it is never actually read by DMA, so it should not eat precious
// internal DMA-capable RAM.
uint16_t g_dummy_band[fluidbox::config::kBandPixels];

// Full-frame back buffer, in PSRAM (OCT, 200 MHz, DMA-capable on this chip).
//
// It cannot live in internal SRAM: 480*480*2 = 460 KB is most of the 512 KB there, and
// the two band buffers plus the USB and QSPI stacks have to fit as well. The panel's own
// bandwidth is unchanged by this -- it is read once per commit, not continuously.
uint16_t* g_frame_buf = nullptr;

// Which bands have been staged since the last commit -- **a bitmap, not a span.**
//
// A span (min..max) is wrong here and the symptom is severe: the PC sends a *diff*, so
// it is entirely normal for band 0 (the timer) and bands 14..15 (the stats strip) to
// arrive with nothing in between. The bounding span then covers rows that were **never
// received**, and pushing it sends the uninitialised frame buffer -- zeros, i.e. black --
// over the middle of the screen. Worse, it is permanent: the PC considers those bands
// unchanged, so it never sends them again.
//
// With a bitmap only rows that actually arrived are ever pushed, and a normal update
// (one contiguous run) is still a single transfer.
uint16_t g_dirty_mask = 0;

// TE rising-edge count. Only incremented in the ISR and read by the statistics,
// so volatile is enough.
volatile uint32_t g_te_edges = 0;

// One token per TE rising edge; the render task uses it to align with the panel
// refresh period.
SemaphoreHandle_t g_te_sem = nullptr;

void IRAM_ATTR TeIsr(void* /*arg*/) {
    // Since C++20, using ++ on a volatile is deprecated (and IDF compiles with
    // -Werror), so use an explicit assignment to sidestep it.
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

// CO5300 power-on sequence: RGB565, brightness and HBM registers on, full window
// set, sleep out, display on.
//
// 0x2A/0x2B are the full 480x480 window (0x0000..0x01DF). The official BSP
// specifically notes that the board-manager default table's 466x466 + offset 6
// handling is wrong, and this is the corrected version.
// CO5300 init sequence.
//
// **This is the one that must be used; the built-in default table of the
// esp_lcd_co5300 component must not be used**, and it must not be assembled by
// intuition either: the component's built-in table is for a different panel
// (address window 472x466), while this Mosaico 480x480 AMOLED has several
// commands that cannot be omitted:
//
//   0x11        sleep out, **must be the very first command**, and must wait
//               600 ms for the panel's internal power rails to come up
//   0xFE=0x20   switch to page 0x20
//   0x19=0x10   panel internal oscillator/power setting  <- without it the
//               screen is pure black the whole time
//   0x1C=0xA0   same as above                             <- without it the
//               screen is pure black the whole time
//   0xFE=0x00   switch back to page 0
//   0x2A/0x2B   address window 0..479
//   0x29        display on, then wait 600 ms
//
// This sequence comes from the Mosaico board code in
// espressif/esp-brookesia
// (hal/brookesia_hal_boards/components/mosaico/brookesia_hal_custom/src/board/display.c,
// whose comment says it was adapted from esp-mosaico-bsp commit bef9967).
// The pitfall we hit: the version we assembled by hand had
// 0xFE/0xC4/0x3A/0x53/0x51/0x63/0x2A/0x2B/0x11/0x29 but was missing 0x19 and
// 0x1C, and on top of that 0x11 was in the wrong place with a 100 ms delay --
// the result was a fully black screen.
const co5300_lcd_init_cmd_t kInitCmds[] = {
    {0x11, nullptr, 0, 600},                                // sleep out, must be first, 600 ms
    {0xFE, (uint8_t[]){0x20}, 1, 0},                        // page 0x20
    {0x19, (uint8_t[]){0x10}, 1, 0},                        // panel power/oscillator
    {0x1C, (uint8_t[]){0xA0}, 1, 0},                        // panel power/oscillator
    {0xFE, (uint8_t[]){0x00}, 1, 0},                        // back to page 0
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},                        // 16 bpp / RGB565
    // TE (tearing synchronisation) output. The official BSP's
    // CONFIG_BSP_CO5300_ENABLE_TE is on by default too.
    // Besides preventing tearing, it also gives us **hardware proof that the
    // panel is alive**: TE is routed to GPIO43, and the panel keeps pulsing as
    // long as it is scanning, whereas QSPI itself offers no feedback at all.
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},                        // brightness control on
    {0x51, (uint8_t[]){0xFF}, 1, 0},                        // full brightness
    {0x63, (uint8_t[]){0xFF}, 1, 0},                        // HBM on
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},      // columns 0..479
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},      // rows 0..479
    {0x29, nullptr, 0, 600},                                // display on, 600 ms
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
    // Screen power: VCC_3V3_CTRL is active low, and it defaults to off at
    // power-up, so the panel does nothing at all until it is pulled low.
    // This is the easiest pitfall to hit on this hardware.
    gpio_config_t power_cfg = {};
    power_cfg.pin_bit_mask = 1ULL << fluidbox::board::kPinVcc3V3Ctrl;
    power_cfg.mode = GPIO_MODE_OUTPUT;
    power_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    power_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&power_cfg), kTag, "vcc ctrl gpio cfg");
    ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(fluidbox::board::kPinVcc3V3Ctrl), 0),
                        kTag, "vcc ctrl low");
    vTaskDelay(pdMS_TO_TICKS(20));  // wait for the 3.3V rail to settle

    // The return value of new is not checked here: CONFIG_COMPILER_CXX_EXCEPTIONS
    // is off, so an allocation failure aborts directly and the check branch would
    // be unreachable -- keeping it would only create the illusion that OOM is
    // already handled.
    // Full-frame back buffer first: without it every band goes out on its own and the
    // picture visibly assembles strip by strip. Failure is not fatal -- StageBand falls
    // back to per-band pushes.
    if (g_frame_buf == nullptr) {
        g_frame_buf = static_cast<uint16_t*>(heap_caps_aligned_alloc(
            64, static_cast<size_t>(fluidbox::config::kLcdHRes) * fluidbox::config::kLcdVRes * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
        if (g_frame_buf == nullptr) {
            ESP_LOGW(kTag, "no PSRAM frame buffer: bands will be pushed one at a time");
        } else {
            ESP_LOGI(kTag, "full-frame staging buffer ready (PSRAM, %d KB)",
                     fluidbox::config::kLcdHRes * fluidbox::config::kLcdVRes * 2 / 1024);
        }
    }

    buffers_free_ = new osal::CountingSemaphore(2, 2);
    g_buffers_free = static_cast<osal::CountingSemaphore*>(buffers_free_);

    // Note: **deliberately not using the two macros
    // CO5300_PANEL_BUS_QSPI_CONFIG / CO5300_PANEL_IO_QSPI_CONFIG**. They are
    // written for C, and expanding them (a) produces a field order that does not
    // match the struct declaration, while C++ designated initializers require
    // exactly matching order, so it simply does not compile; and (b) assigns an
    // int to the gpio_num_t enum. Assigning field by field sidesteps both
    // pitfalls and reads better too.
    spi_bus_config_t bus_config = {};
    bus_config.sclk_io_num = fluidbox::board::Active().lcd_pclk;
    bus_config.data0_io_num = fluidbox::board::kPinLcdData0;
    bus_config.data1_io_num = fluidbox::board::kPinLcdData1;
    bus_config.data2_io_num = fluidbox::board::kPinLcdData2;
    bus_config.data3_io_num = fluidbox::board::kPinLcdData3;
    // **The largest transfer the panel will ever be asked for is a whole frame**, because
    // a staged update is pushed as one run (see CommitFrame).
    //
    // This used to be exactly one band, which was correct while every band was pushed on
    // its own -- and became a silent ceiling the moment updates started being coalesced.
    // Anything larger fails to queue:
    //     E lcd_panel.io.spi: spi transmit (queue) color failed
    //     E co5300_spi: send color data failed
    // The visible result is a screen that only ever shows the bands small enough to fit,
    // with the rest left black -- and because the PC diffs against what it already sent,
    // those rows were never sent again.
    bus_config.max_transfer_sz =
        fluidbox::config::kLcdHRes * fluidbox::config::kLcdVRes * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(static_cast<spi_host_device_t>(kLcdHost),
                                           &bus_config, SPI_DMA_CH_AUTO),
                        kTag, "spi bus init failed");

    // The 5 QSPI lines need their drive capability set explicitly -- this is not
    // optional; the official Mosaico board code does exactly this step
    // (configure_display_drive_capability) before creating the panel.
    //
    // The official code uses the weakest setting (0) by default in order to lower
    // edge rates and EMI, but its own comment also states "if signal integrity
    // fails at high clock, step it up". This project defaults to 2 in order to
    // leave setup-time margin at 60 MHz. **Previously, the weakest setting with
    // 80 MHz caused occasional bit errors, showing up as "one band flashes some
    // other colour", and since QSPI is write-only the host side could not see it
    // at all.**
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

    // The QSPI mode parameters match the driver defaults, except that the
    // command/parameter bit widths are 32/8 and we pick pclk ourselves (the
    // default is 40 MHz, here it is raised to 60).
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = static_cast<gpio_num_t>(fluidbox::board::kPinLcdCs);
    io_config.dc_gpio_num = static_cast<gpio_num_t>(-1);  // QSPI needs no DC line
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

    // Wire the TE pin as an input and count edges. **This is not for tearing
    // prevention** (this project does not use it), but to obtain the hardware
    // evidence that "the panel really is scanning" -- QSPI is write-only, and
    // without this there is nothing but the eye to go on.
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
            // Installing twice returns ESP_ERR_INVALID_STATE; ignoring it is fine.
            (void)gpio_install_isr_service(0);
            (void)gpio_isr_handler_add(static_cast<gpio_num_t>(fluidbox::board::kPinLcdTe),
                                       TeIsr, nullptr);
            ESP_LOGI(kTag, "TE counting enabled (GPIO%d)", fluidbox::board::kPinLcdTe);
        }
    }

    ESP_LOGI(kTag, "CO5300 %dx%d @ %d MHz ready (board %s)", fluidbox::config::kLcdHRes,
             fluidbox::config::kLcdVRes, kLcdPixelClockHz / 1000000, fluidbox::board::Active().name);
    return ESP_OK;
}

esp_err_t Panel::SetBrightness(uint8_t level) {
    if (panel_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // Go through the API the driver provides rather than writing a raw byte to
    // 0x51 ourselves: it validates the parameter range, converts the percentage
    // into the hardware value, and logs what was actually written, which is
    // useful when diagnosing "the screen is too dark/too bright".
    //
    // Our interface is 0..255 (same scale as the register, more intuitive when
    // tuning), while the driver wants 0..100, so convert here.
    const unsigned percent =
        (static_cast<unsigned>(level) * 100u + 127u) / 255u;  // round to nearest
    return esp_lcd_panel_co5300_set_brightness(
        static_cast<esp_lcd_panel_handle_t>(panel_),
        static_cast<uint8_t>(percent > 100u ? 100u : percent));
}

uint16_t* Panel::AcquireBand() {
    auto* sem = static_cast<osal::CountingSemaphore*>(buffers_free_);
    if (sem == nullptr) {
        // The panel is not ready (FBOX_SKIP_PANEL, or Init failed midway).
        // A null pointer must never be dereferenced here -- that would turn
        // "no panel" into "system crash and reboot".
        return g_dummy_band;
    }
    if (!sem->Take(kAcquireTimeoutMs)) {
        // The transfer-complete interrupt never came back (most likely FlushBand
        // failed to queue at some point).
        // Drop this frame and leave evidence behind; never wait forever, because
        // waiting forever is a silent frozen screen, which is far harder to
        // diagnose than a dropped frame.
        ESP_LOGE(kTag, "band buffer wait timed out (transfer-complete interrupt never came back?), dropping this frame");
        return g_dummy_band;
    }
    return g_band_buf[next_buffer_++ & 1u];
}

esp_err_t Panel::StageBand(int band_index, const uint16_t* buffer) {
    if (panel_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    auto* sem = static_cast<osal::CountingSemaphore*>(buffers_free_);
    if (g_frame_buf == nullptr) {
        // No staging buffer (PSRAM allocation failed): fall back to pushing the band on
        // its own. Strippy is better than blank.
        const esp_err_t err = FlushBand(band_index, buffer);
        if (sem != nullptr) {
            sem->Give();
        }
        return err;
    }
    const int y0 = band_index * fluidbox::config::kBandRows;
    memcpy(g_frame_buf + static_cast<size_t>(y0) * fluidbox::config::kLcdHRes, buffer,
           fluidbox::config::kBandPixels * 2);
    g_dirty_mask = static_cast<uint16_t>(
        g_dirty_mask | (1u << band_index));
    // The pixels are copied, not queued: the band buffer is free again immediately.
    // Without this the pool would drain after two bands and AcquireBand would start
    // timing out.
    if (sem != nullptr) {
        sem->Give();
    }
    return ESP_OK;
}

bool Panel::HasStaged() const { return g_dirty_mask != 0; }

esp_err_t Panel::CommitFrame() {
    if (panel_ == nullptr || g_frame_buf == nullptr || g_dirty_mask == 0) {
        return ESP_ERR_INVALID_STATE;   // nothing staged, nothing to do
    }
    const uint16_t mask = g_dirty_mask;
    g_dirty_mask = 0;

    // Align with the panel's scanout before starting. This is the half that removes the
    // visible seam; a single transfer alone would still begin at an arbitrary point in
    // the scan.
    (void)WaitForTeTick(20);

    // One transfer per **contiguous run** of staged bands. A whole-screen update is one
    // run and therefore one transfer, which is the case that matters; a genuinely
    // scattered update becomes two or three, and that is correct -- the alternative is
    // pushing rows this board has no pixels for.
    esp_err_t err = ESP_OK;
    int band = 0;
    while (band < fluidbox::config::kBandCount) {
        if ((mask & (1u << band)) == 0) {
            band++;
            continue;
        }
        int run = band;
        while (run < fluidbox::config::kBandCount && (mask & (1u << run)) != 0) {
            run++;
        }
        const int y0 = band * fluidbox::config::kBandRows;
        const int y1 = run * fluidbox::config::kBandRows;
        err = esp_lcd_panel_draw_bitmap(
            static_cast<esp_lcd_panel_handle_t>(panel_), 0, y0,
            fluidbox::config::kLcdHRes, y1,
            g_frame_buf + static_cast<size_t>(y0) * fluidbox::config::kLcdHRes);
        if (err != ESP_OK) {
            break;
        }
        band = run;
    }
    return err;
}

esp_err_t Panel::FlushBand(int band_index, const uint16_t* buffer) {
    if (panel_ == nullptr) {
        // Panel not ready: dropping the content is fine, this is not an error.
        return ESP_ERR_INVALID_STATE;
    }
    const int y0 = band_index * fluidbox::config::kBandRows;
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        static_cast<esp_lcd_panel_handle_t>(panel_), 0, y0,
        fluidbox::config::kLcdHRes, y0 + fluidbox::config::kBandRows, buffer);
    if (err != ESP_OK) {
        // Queueing failed -> the completion interrupt will never fire -> this
        // buffer can never come back.
        // It must be returned here, otherwise after two losses in a row the
        // render task would wait forever for a buffer.
        auto* sem = static_cast<osal::CountingSemaphore*>(buffers_free_);
        if (sem != nullptr) {
            sem->Give();
        }
        ESP_LOGE(kTag, "band %d queue failed: %s (buffer returned)", band_index,
                 esp_err_to_name(err));
    }
    return err;
}

uint32_t Panel::TeEdges() const { return g_te_edges; }

bool Panel::WaitForTeTick(uint32_t timeout_ms) {
    if (g_te_sem == nullptr) {
        return false;
    }
    // Clear any stale tokens that may have piled up first, to make sure we wait
    // for the "next" edge.
    (void)xSemaphoreTake(g_te_sem, 0);
    return xSemaphoreTake(g_te_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void Panel::PrepareForBootloader() {
    if (panel_ == nullptr) {
        return;
    }
    // Turn the display off first and then wait -- the bus is write-only, so it
    // cannot be confirmed; just allow time for the worst case.
    (void)esp_lcd_panel_disp_on_off(static_cast<esp_lcd_panel_handle_t>(panel_), false);
    (void)SetBrightness(0);
    vTaskDelay(pdMS_TO_TICKS(50));
}

}  // namespace display
