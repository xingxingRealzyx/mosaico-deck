// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// display -- CO5300 QSPI AMOLED panel, band double buffering + asynchronous DMA
//
// There is no full-frame buffer
// ----------------------------
// One frame is 480x480x2 = 450 KB, and internal SRAM cannot hold two of them;
// putting them in PSRAM would instead make the CPU writing pixels and the DMA
// reading pixels fight over the same external bus.
//
// So the screen is drawn as **16 horizontal bands of 30 rows** each. A band is
// 28800 bytes, and two of them fit comfortably in internal SRAM. The loop is:
//
//     for each band:
//         wait until this band's buffer is free
//         clear it to black
//         draw every particle that intersects this band
//         hand it to DMA and immediately continue with the next band
//
// esp_lcd_panel_draw_bitmap() only queues the transfer and returns, so while the
// CPU draws band N, band N-1 is still in flight. The counting semaphore starts
// at 2 and is released by the transfer-complete interrupt; that is exactly how
// "wait until the buffer is free" works: taking the semaphore means at least one
// buffer is free, and transfers complete in queue order, so the free one is
// necessarily the one to be reused next.
//
// The panel link is the ceiling. QSPI moves 4 bit per clock, so one 450 KB frame
// takes about 11.3 ms at 80 MHz. The GP-SPI host on the S31 tops out at 80 MHz;
// the official BSP conservatively uses 40 MHz.

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

    // QSPI bus + panel IO + CO5300 init + display on.
    esp_err_t Init();

    // 0..255. The AMOLED dims itself; there is no backlight pin.
    esp_err_t SetBrightness(uint8_t level);

    // **Display access is not thread-safe**: two tasks doing "take buffer -> draw
    // -> submit" at the same time will fight over the SPI bus and report
    // `acquire spi bus failed` / `ESP_ERR_INVALID_STATE`.
    //
    // Normally "the screen has exactly one owner" guarantees this (once the PC
    // takes over, the local pattern stops), but the transition instant can still
    // collide. This lock is the safety net for that moment.
    //
    // Usage: the guard returned by DrawLock() must live across the whole
    // Acquire -> draw -> Flush sequence.
    [[nodiscard]] osal::Mutex& DrawMutex() { return draw_mutex_; }

    // Block until a band buffer is free, then return it.
    // The size is kBandRows * kLcdHRes pixels.
    //
    // Buffers rotate by "number of requests" rather than by band index: the
    // renderer skips bands that are already black on the panel, and taking them
    // by index would hand the same buffer to two transfers still in flight.
    //
    // **It does not return a null pointer when the panel is not ready.** Instead
    // it returns a dummy buffer (the renderer draws into it as usual and
    // FlushBand simply discards it). Diagnostic mode FBOX_SKIP_PANEL takes
    // exactly this path -- if this function dereferenced a null pointer, that
    // "unbrick" path would itself crash, which is as good as not having it.
    //
    // The wait has a timeout: if the transfer-complete interrupt never comes back
    // (FlushBand failed to queue at some point), it is better to drop a frame and
    // log it than to wait forever and freeze the screen.
    uint16_t* AcquireBand();

    // Queue an asynchronous transfer for one band. Does not block.
    // If queueing fails the buffer is returned -- otherwise the completion
    // interrupt never arrives and that buffer is lost forever; after losing two
    // in a row, AcquireBand would block indefinitely.
    esp_err_t FlushBand(int band_index, const uint16_t* buffer);

    // The panel is really usable: Init succeeded, there is a panel handle, and
    // there is also a buffer semaphore.
    bool ready() const { return panel_ != nullptr && buffers_free_ != nullptr; }

    // Cumulative number of TE (tearing synchronisation) rising edges.
    //
    // **This is the only evidence of "is the panel actually alive" that does not
    // rely on the human eye.** QSPI is a write-only bus -- whether a command was
    // accepted by the panel, and whether the panel is scanning, is completely
    // invisible to the host from the bus. But TE is a signal the panel **outputs**
    // to the host: as long as the panel is powered, correctly initialised, and
    // scanning, it keeps emitting pulses at the refresh rate. Reading it separates
    // "the panel is dead" from "the panel is alive, we just did not see it".
    //
    // Requires the 0x35 (TE ON) command during init, plus GPIO43 configured as an
    // input.
    uint32_t TeEdges() const;

    // Block until the next TE rising edge, waiting at most timeout_ms. Returns
    // true if it really caught one.
    //
    // **This is frame synchronisation.** The panel's refresh rate (about 58.5 Hz)
    // is the real ceiling of this system; pushing faster than that does not make
    // the picture smoother, it only writes into the panel's GRAM while it is
    // scanning some region -- in which case **an entire band may be rejected for
    // the whole frame**, which looks like "one particular band never refreshes".
    // And this kind of collision is completely invisible on the host side (QSPI is
    // write-only and the driver reports ESP_OK throughout).
    //
    // The official BSP does the same (CONFIG_CO5300_ENABLE_TE + TE_SYNC).
    bool WaitForTeTick(uint32_t timeout_ms);

    // Display off + brightness to zero, to drop the AMOLED current to a minimum.
    // Called before entering ROM download mode -- GPIO state is lost after a
    // reboot, and the ROM does not drive the screen's 3.3V control pin either; if
    // the screen were still lit, flashing would superimpose that on the write
    // current spikes.
    void PrepareForBootloader();

    // Upper bound on the buffer wait. A normal 28800-byte transfer takes about
    // 1.4 ms at 80 MHz, so 500 ms is far beyond "normal" and can only mean the
    // interrupt did not come back.
    static constexpr uint32_t kAcquireTimeoutMs = 500;

 private:
    void* panel_ = nullptr;  // esp_lcd_panel_handle_t
    void* io_ = nullptr;     // esp_lcd_panel_io_handle_t
    void* buffers_free_ = nullptr;  // osal::CountingSemaphore*
    osal::Mutex draw_mutex_;
    unsigned next_buffer_ = 0;
};

}  // namespace display
