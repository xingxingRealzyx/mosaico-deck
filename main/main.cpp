// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// mosaico-deck -- a desktop agent status/interaction terminal
//
// Architecture (why the UI is not rendered on this board)
// ------------------------------------------------------
// The board does only three things: **display**, **collect input**, and **talk to the PC**.
// The UI (text, icons, layout, colors) is entirely rendered on the PC side and sent over as bands.
//
// The reason is not "the board is too slow", it is **development speed**:
//   - CJK text would need a hand-made bitmap font (1~2MB, and it looks bad); the PC has system fonts, perfect and free
//   - Antialiasing, icons, emoji -- all free on the PC
//   - Changing the UI once: on the PC side saving the file takes effect immediately, on the board side it means reflashing
// That last point is an order-of-magnitude difference. The less code on the board side, the faster "tweaking the UI" gets.
//
// On top of that, the board is connected to the computer over USB, so bandwidth and power are both non-issues --
// a whole 450KB full-screen frame takes only 0.5 seconds to transfer; sending just the changed bands is a few KB, tens of milliseconds.

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

// Speaker volume at boot, 0-100.
//
// **Nothing ever set this.** The codec played at its power-on default, which is why
// speech was too quiet to hear from across a desk -- the effect was there the whole time,
// it was just never turned up.
inline constexpr int kDefaultVolume = 85;


constexpr const char* kTag = "deck";

display::Panel g_panel;
fluidbox::touch::Touchscreen g_touch;
input::ResetButton g_button;
i2c_master_bus_handle_t g_i2c_bus = nullptr;

uint32_t g_frames = 0;
uint32_t g_band_flushes = 0;

// ---------------------------------------------------------------------------
// Power: the screen's 3.3V control pin
// ---------------------------------------------------------------------------
// GPIO60 controls the screen supply and the external 3.3V output, **active low**. It is off at power-on,
// so unless we actively drive it low the panel shows nothing at all -- and since QSPI is a write-only bus,
// the host side cannot tell anything is wrong.
esp_err_t PowerOnRails() {
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << fluidbox::board::kPinVcc3V3Ctrl,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), kTag, "failed to configure GPIO%d",
                        fluidbox::board::kPinVcc3V3Ctrl);
    // Set the level before enabling the output, so no glitch appears at power-on.
    ESP_RETURN_ON_ERROR(
        gpio_set_level(static_cast<gpio_num_t>(fluidbox::board::kPinVcc3V3Ctrl), 0), kTag,
                        "failed to drive VCC_3V3 low");
    // The supply needs a little time to ramp up. The official code uses LEDC as a ramp; here we just wait --
    // the only difference is the inrush at power-on, which does not affect functionality.
    osal::Delay(120);
    ESP_LOGI(kTag, "screen 3.3V enabled (GPIO%d=0)",
             fluidbox::board::kPinVcc3V3Ctrl);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2C pin investigation
// ---------------------------------------------------------------------------
// **The documentation and the actual measurements disagree.** The official V1.0 docs say the internal I2C0 is on
// GPIO0/1, but on this board nothing at all is found there; GPIO56/3 however has 6 devices (touch, IMU,
// magnetometer, fuel gauge, codec). So at boot we scan both sets and use the one that has devices.
//
// Incidentally this is the **second independent signal** for determining the board revision: V1.0 has I2C0 on
// GPIO0/1, V1.2 on GPIO56/3.
int ScanBusOnPins(int sda, int scl, uint32_t hz) {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_1;  // scan on port 1 and release it afterwards, so the real work is unaffected
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
            ESP_LOGI(kTag, "  I2C(%d,%d) found device 0x%02X", sda, scl, addr);
            found++;
        }
    }
    i2c_del_master_bus(bus);
    return found;
}

// ---------------------------------------------------------------------------
// Display pipeline self-test
// ---------------------------------------------------------------------------
// This version has no PC frames yet, so it draws a pattern to prove the whole pipeline works: **are all 16 bands
// updating, is the color byte order right, is any band being skipped**.
//
// Pattern: a full-screen vertical gradient (every band has a different base color -> a skipped band shows up as a
// hard edge), plus a bright bar sweeping horizontally (proving it keeps refreshing), plus a white square in each
// corner (proving the boundaries are covered).
// ---------------------------------------------------------------------------
// Display pipeline self-test
// ---------------------------------------------------------------------------
// Key point: **only redraw the bands that changed, do not push the whole screen.**
//
// Why this matters: a full 450KB screen takes 15.4ms over 60MHz QSPI, while the panel refresh period is 17ms
// -- one frame's transfer time is almost equal to one refresh, so any jitter crosses the boundary and the top half
// shows new content while the bottom half is still old, which looks like tearing (in practice a sweeping bright bar
// "sometimes bends").
//
// A real UI is static most of the time and only a few bands change: a text update is 1~3 bands,
// which takes 1.4~4.2ms, comfortably inside one refresh, so **it does not tear at all**.
//
// So the right fix is not "slow the animation down" (that only hides the problem), it is to send only the parts that changed.
//
// ---------------------------------------------------------------------------
// Boot animation: the DeepSeek Harness whale logo -> shatters into particles that scatter -> hand over to the PC
// ---------------------------------------------------------------------------
// **The animation has to run on the board.** If the PC pushed frames, one frame is 16 bands / 450KB and the transfer
// alone takes 15.4ms; adding the render round-trip makes smooth motion impossible. Drawing locally on the board is
// limited only by QSPI bandwidth and reaches ~40fps in practice, which is plenty.
//
// Three phases:
//   1. Fade in   (0.0~0.7s)  the logo fades from dark to bright
//   2. Scatter   (0.7~2.2s)  the logo's pixels become particles that fly outwards and fade
//   3. Handover  (after 2.2s) a black screen -- once the PC sends its first frame we yield forever
//
// Particle positions are computed analytically (p = p0 + v*t) instead of accumulated frame by frame: first, there is
// no floating-point drift; second, it can be evaluated directly at any instant, unaffected by dropped frames.
namespace boot {

using fluidbox::config::kBandCount;
using fluidbox::config::kBandPixels;
using fluidbox::config::kBandRows;
using fluidbox::config::kLcdHRes;
using fluidbox::config::kLcdVRes;

constexpr int64_t kFadeInUs = 600 * 1000;    // phase 1: fade in
constexpr int64_t kScatterUs = 1100 * 1000;  // phase 2: scatter
// Phase 3: the afterglow: the animation no longer "ends"; instead it settles into a quiet state and **stays there**,
// until the PC sends its first frame.
//
// We got this wrong once: originally the animation ended hard at 2.2s and then showed black while waiting for the PC.
// But the particles each have their own launch delay and the last one faded out at ~1.7s, so the final 0.5s was pure
// black; adding the ~1.2s the PC side needs to establish the connection, what the user saw was "after it explodes,
// the screen stays black for quite a while".
//
// Now the animation has no "finished" state -- there is always something on screen, and when the PC arrives it takes over naturally.
constexpr int64_t kScatterEndUs = kFadeInUs + kScatterUs;

// center the logo
constexpr int kLogoX = (kLcdHRes - deck::kLogoSize) / 2;
constexpr int kLogoY = (kLcdVRes - deck::kLogoSize) / 2;

// Sampling interval: take one particle every kSample pixels in the mask.
// 3 -> about 1100 particles, dense enough when scattering, and not too many to compute per frame.
constexpr int kSample = 3;
constexpr int kMaxParticles = (deck::kLogoSize / kSample) * (deck::kLogoSize / kSample);

struct Particle {
    float x0, y0;   // start point (relative to the screen center)
    float vx, vy;   // velocity (pixels/second)
    float delay;    // per-particle launch delay, so the scatter has depth instead of translating as one block
};

Particle g_particles[kMaxParticles];
int g_count = 0;

// A deterministic pseudo-random number -- we avoid rand() because it needs srand and its result depends on the libc
// version, so switching toolchains would change the boot animation. This uses an integer hash, so the result is always the same.
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
                continue;  // not part of the logo
            }
            if (g_count >= kMaxParticles) {
                break;
            }
            const float dx = static_cast<float>(x - half);
            const float dy = static_cast<float>(y - half);
            // The farther from the center, the faster it flies -- visually this reads as "exploding from the middle"
            const float dist = sqrtf(dx * dx + dy * dy) + 1.0f;
            const uint32_t h = Hash(static_cast<uint32_t>(y * 1000 + x));
            const float jitter = 0.75f + static_cast<float>(h % 1000) / 1000.0f * 0.7f;
            const float speed = (90.0f + dist * 1.9f) * jitter;

            Particle& p = g_particles[g_count++];
            p.x0 = dx;
            p.y0 = dy;
            p.vx = dx / dist * speed;
            p.vy = dy / dist * speed;
            // delay 0~180ms, spread out by hash
            p.delay = static_cast<float>((h >> 10) % 180) / 1000.0f;
        }
    }
    ESP_LOGI(kTag, "boot animation: %d particles", g_count);
}

// Fill one row of pixels of a band with black (RGB565 with the high byte first, same as the PC side)
inline uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return static_cast<uint16_t>((v << 8) | (v >> 8));  // byte swap
}

const uint16_t kBlack = Rgb565(0, 0, 0);

// Draw one frame. With full_redraw all 16 bands are drawn (the first frame initializes the whole screen).
//
// **This function has no "end"**: elapsed_us keeps growing and what is drawn naturally transitions from "scatter" to
// "afterglow", so there is always content. Whether the PC has taken over is decided by the render loop, not here.
void DrawFrame(int64_t elapsed_us, bool full_redraw) {
    InitParticles();
    const bool scattering = elapsed_us >= kFadeInUs;
    const float st = static_cast<float>(elapsed_us - kFadeInUs) / 1e6f;
    // seconds since the end of the scatter (used by the afterglow phase)
    const float after = (elapsed_us > kScatterEndUs)
                            ? static_cast<float>(elapsed_us - kScatterEndUs) / 1e6f
                            : 0.0f;

    for (int band = 0; band < kBandCount; band++) {
        const int y0 = band * kBandRows;
        uint16_t* buf = g_panel.AcquireBand();
        if (buf == nullptr) {
            return;  // timed out, drop this frame (try again next frame)
        }
        for (int i = 0; i < kBandPixels; i++) {
            buf[i] = kBlack;
        }

        if (!scattering) {
            // ---- phase 1: logo fades in ----
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
            // ---- phases 2 and 3: particles scatter -> afterglow ----
            // Each particle is drawn as 2x2, so from a distance there is still "something there" rather than sparse dots.
            //
            // The brightness decay used (1 - local/1.5)^2, so the latest particle (delay at most 0.18s)
            // went fully dark at st~1.68s -- while the scatter phase is 1.1s, meaning **the particles became invisible
            // before they finished flying**. That is half the reason for the "black stretch after the explosion".
            // The fade denominator is now stretched to match the whole scatter duration, and the afterglow phase keeps
            // a little residual light.
            for (int i = 0; i < g_count; i++) {
                const Particle& p = g_particles[i];
                const float local = st - p.delay;
                if (local <= 0.0f) {
                    continue;
                }
                float fade = 1.0f - local / 1.35f;
                if (fade <= 0.0f) {
                    // Afterglow: only a very few particles are still drifting slowly outwards, leaving the screen a little "breathing"
                    // Selected with (i % 23), about 4%, so it looks like occasional stars rather than noise
                    if ((i % 23) != 0) {
                        continue;
                    }
                    fade = 0.16f + 0.10f * sinf(after * 1.6f + static_cast<float>(i));
                    if (fade <= 0.0f) {
                        continue;
                    }
                    const float slow = local * 0.35f;  // almost frozen during the afterglow phase
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
                // Scatter phase: brightness decays quadratically, so the tail ends cleanly
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
// Tasks
// ---------------------------------------------------------------------------
void RenderTask(void* arg) {
    (void)arg;
    ESP_LOGI(kTag, "render task started");

    // The local self-test pattern is only drawn while **the PC has not taken over the screen yet**.
    //
    // Why yielding is mandatory: two tasks drawing at the same time fight over the SPI bus (measured as
    // `acquire spi bus failed` + `ESP_ERR_INVALID_STATE`), and once the consumer task is blocked on the bus it has no
    // time to read the ring buffer -- which shows up as heavy RX overflow. The screen can only have one owner.
    const int64_t boot_start_us = osal::TimeUs();
    bool first = true;
    bool gave_up = false;
    bool anim_done = false;
    for (;;) {
        if (hostlink::RemoteOwnsScreen()) {
            if (!gave_up) {
                gave_up = true;
                ESP_LOGI(kTag, "PC has taken over the screen, boot animation yields");
            }
            osal::Delay(50);
            continue;
        }

        // **The animation has no "end"; it draws until the PC takes over.**
        //
        // It used to stop hard at 2.2s and show black waiting for the PC, which produced "a long black stretch after
        // the explosion" -- because the particles carry delays, the last one faded at 1.7s, so the remaining time was
        // pure black; on top of the time the PC needs to establish the connection it got even longer. Now the
        // animation settles into a quiet afterglow state that keeps the screen company, and when the PC arrives it is
        // naturally taken over.
        //
        // The one rule to keep: **as soon as the PC takes over we must stop immediately** -- drawing on both sides at
        // once fights over the SPI bus (measured as acquire spi bus failed), and a blocked consumer task causes RX
        // overflow. The RemoteOwnsScreen() check at the top of the loop guarantees this.
        const int64_t elapsed = osal::TimeUs() - boot_start_us;
        if (!anim_done && elapsed > boot::kScatterEndUs + 1500 * 1000) {
            anim_done = true;
            ESP_LOGI(kTag, "boot animation entering afterglow (%.1fs)", elapsed / 1e6);
        }

        // Wait for one panel refresh period before pushing a frame. The panel refresh rate (~58.5Hz) is the real
        // ceiling of this system; pushing faster does not make it smoother, it only writes into GRAM while the panel
        // is scanning -- in which case an entire band may never be accepted for the whole frame.
        if (g_panel.WaitForTeTick(30)) {
            // The same lock the link contends for -- see display::Panel::DrawMutex().
            osal::LockGuard<osal::Mutex> draw_guard(g_panel.DrawMutex());
            boot::DrawFrame(elapsed, first);
            first = false;
            g_frames = g_frames + 1;
        }
    }
}

// Touch reporting. **Only send on press/release edges** -- touch is polled every 3ms, so reporting every sample would
// be 300 messages per second and would flood USB, while what the PC really needs is just "where did the finger land,
// and when did it lift".
void TouchTask(void* arg) {
    (void)arg;
    bool was_active = false;
    bool rec_held = false;
    int last_x = -1;
    int last_y = -1;
    int64_t last_move_us = 0;
    int64_t last_button_us = osal::TimeUs();
    for (;;) {
        g_touch.Poll();
        const fluidbox::touch::State ts = g_touch.Get();
        if (ts.active != was_active) {
            was_active = ts.active;
            last_x = ts.x;
            last_y = ts.y;
            hostlink::SendTouch(ts.active, ts.x, ts.y);
            ESP_LOGI(kTag, "touch %s (%d,%d)", ts.active ? "down" : "up", ts.x, ts.y);
        } else if (ts.active) {
            // Movement while the finger is down. Touch is polled every 3ms and the PC
            // only needs enough samples to follow a finger, so report on real movement
            // (3px) at a bounded rate -- this is what makes dragging a slider and the
            // pull-down gesture possible at all.
            const int64_t move_us = osal::TimeUs();
            const int dx = ts.x > last_x ? ts.x - last_x : last_x - ts.x;
            const int dy = ts.y > last_y ? ts.y - last_y : last_y - ts.y;
            if ((dx >= 3 || dy >= 3) && move_us - last_move_us >= 25 * 1000) {
                last_x = ts.x;
                last_y = ts.y;
                last_move_us = move_us;
                hostlink::SendTouchMove(ts.x, ts.y);
            }
        }

        // The button is polled here too -- spinning up a separate task just to read a GPIO every 25ms is not worth it.
        //
        // The two uses of the button:
        //   - **hold = push to talk** (the most natural interaction: speak whenever you want, no hunting for an on-screen button)
        //   - short press = cancel the current query (handled on the PC side)
        const int64_t now = osal::TimeUs();
        if (now - last_button_us >= 25 * 1000) {
            last_button_us = now;

            // Check the level first: while held, keep recording.
            const bool held = g_button.IsPressed();
            if (held != rec_held) {
                rec_held = held;
                if (held) {
                    hostlink::StartCapture();
                    ESP_LOGI(kTag, "button pressed -> start recording");
                } else {
                    hostlink::StopCapture();
                    ESP_LOGI(kTag, "button released -> stop recording");
                    hostlink::SendKey("voice_end");
                }
                // Presses/releases during recording are not reported as short/long presses, otherwise the PC side
                // would receive a "short press" and then an audio clip, which is semantically contradictory.
                (void)g_button.Poll();
            } else {
                switch (g_button.Poll()) {
                    case input::ResetButton::Event::kShortPress:
                        hostlink::SendKey("short");
                        ESP_LOGI(kTag, "button: short press");
                        break;
                    case input::ResetButton::Event::kLongPress:
                        hostlink::SendKey("long");
                        ESP_LOGI(kTag, "button: long press");
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
        // df/dt and db/dt -- **both numbers must be divided by dt**.
        // Previously db was printed raw, so 1872, which is a "count per 2 seconds", looked like a frequency and did
        // not match 58.5 fps (1872/58.5 is not an integer). After dividing by dt it is 936 band/s
        // = 58.5 fps x 16 bands, which finally adds up.
        const hostlink::Stats ls = hostlink::GetStats();
        ESP_LOGI(kTag, "audio playback%u record%u blocks | %s",
                 static_cast<unsigned>(ls.audio_in), static_cast<unsigned>(ls.audio_out),
                 hostlink::Capturing() ? "recording" : "idle");
        ESP_LOGI(kTag, "link frames%u band%u full%u ctrl%u | bad magic%u bad len%u timeout%u | RX overflow%u",
                 static_cast<unsigned>(ls.frames), static_cast<unsigned>(ls.bands),
                 static_cast<unsigned>(ls.full), static_cast<unsigned>(ls.ctrl),
                 static_cast<unsigned>(ls.bad_magic), static_cast<unsigned>(ls.bad_len),
                 static_cast<unsigned>(ls.rx_timeout),
                 static_cast<unsigned>(usb_console::RxOverflowCount()));
        ESP_LOGI(kTag, "%.1f fps | %.0f band/s | TE %u | touch %s(%d,%d) | SRAM %u B",
                 static_cast<double>(df) / dt, static_cast<double>(db) / dt,
                 static_cast<unsigned>(g_panel.TeEdges()),
                 ts.active ? "down" : "up", ts.x, ts.y,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    }
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "===== mosaico-deck starting =====");

    // ---- board revision: must be done first ----
    // Read the eFuse to decide LCD_RST/LCD_SCL and the I2C pins. V1.0 and V1.2 swap these two sets, and guessing
    // wrong means "the panel initializes successfully but the screen stays black the whole time" (QSPI is write-only,
    // so the host has no idea).
    fluidbox::board::Init();

    // ---- USB console ----
    // Do this early: all the logging that follows depends on it. Note that USB enumeration takes a few hundred
    // milliseconds, so logs before that are lost (the official SDK behaves the same way).
    if (usb_console::Init() != ESP_OK) {
        ESP_LOGW(kTag, "USB console init failed");
    }
    usb_console::AttachToLogger();

    // ---- screen power ----
    if (PowerOnRails() != ESP_OK) {
        ESP_LOGE(kTag, "3.3V power-on failed");
    }

    // ---- I2C pin investigation ----
    const int n_v10 = ScanBusOnPins(0, 1, 100000);
    const int n_v12 = ScanBusOnPins(56, 3, 100000);
    // Report to board first: when the eFuse cannot be trusted, this is the second independent signal for the board revision.
    fluidbox::board::SetRevisionFromI2cProbe(n_v12 > n_v10);
    ESP_LOGI(kTag, "I2C probe: V1.0 set %d devices / V1.2 set %d devices", n_v10, n_v12);

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
        ESP_LOGE(kTag, "I2C bus creation failed (SDA=%d SCL=%d)", sda, scl);
        g_i2c_bus = nullptr;
    }

    // ---- panel ----
    if (g_panel.Init() != ESP_OK) {
        ESP_LOGE(kTag, "panel init failed");
    } else {
        g_panel.SetBrightness(fluidbox::config::kDisplayBrightness);
        ESP_LOGI(kTag, "panel ready, brightness %d", fluidbox::config::kDisplayBrightness);
    }

    // ---- touch ----
    if (g_i2c_bus != nullptr) {
        if (g_touch.Init(g_i2c_bus) != ESP_OK) {
            ESP_LOGW(kTag, "touch init failed, display is unaffected");
        }
    }

    // ---- button ----
    if (g_button.Init() != ESP_OK) {
        ESP_LOGW(kTag, "button init failed");
    }

    // Turn the screen off before entering ROM download mode: after a reboot the GPIO state is lost and the ROM does
    // not drive the screen's 3.3V control pin either, so if the screen is still on, the current spike from flashing
    // can collapse the supply.
    usb_console::SetPreRestartHook([]() { g_panel.PrepareForBootloader(); });

    // ---- audio ----
    // The ES8311 sits on I2C, so this must come after the I2C bus is up.
    // Failure does not affect display or touch, it just means no speaking/listening.
    if (g_i2c_bus != nullptr) {
        if (audio::Init(g_i2c_bus) != ESP_OK) {
            ESP_LOGW(kTag, "audio init failed, the device can neither speak nor listen");
        } else {
            // Set an audible default: see kDefaultVolume.
            audio::SetVolume(kDefaultVolume);
        }
    }

    // ---- link ----
    // SetPanel must come before Init -- link does not know where the panel comes from, main decides that.
    // Passing nullptr when the panel is not ready is also legal: link keeps receiving frames and simply does not
    // display them (otherwise the stream would get out of sync).
    hostlink::SetPanel(g_panel.ready() ? &g_panel : nullptr);

    // Commands from the PC. link parsed the wire; **main decides what they mean**, so
    // link never has to know that audio or display exist. Both take 0-100 on the wire
    // even though the panel's own scale is 0-255: one scale at the boundary, converted
    // once, here.
    hostlink::SetCtrlSink([](const char* cmd, int value) -> bool {
        if (value < 0 || value > 100) {
            return false;
        }
        if (strcmp(cmd, "volume") == 0) {
            return audio::SetVolume(value) == ESP_OK;
        }
        if (strcmp(cmd, "commit") == 0) {
            // The PC has finished sending this update's bands; put them on the glass as
            // one transfer. The draw mutex is shared with the band path, which runs on
            // the same task but on a different message.
            hostlink::RequestCommit();   // asynchronous: see RequestCommit
            return true;
        }
        if (strcmp(cmd, "brightness") == 0) {
            return g_panel.SetBrightness(static_cast<uint8_t>(value * 255 / 100)) == ESP_OK;
        }
        return false;
    });

    hostlink::Init();

    // ---- start tasks ----
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

    ESP_LOGI(kTag, "startup complete. Now waiting for PC frames; local self-test pattern: gradient + sweeping bright bar + corner blocks");
}
