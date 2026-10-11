// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// usb_console implementation. See the rationale in include/usb_console.h.

#include "usb_console.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "vfs_tinyusb.h"

namespace usb_console {
namespace {

constexpr const char* kTag = "usb_console";
// Two CDC ports, deliberately split by role:
//
//   kFlashItf (port 0) -- owns the control-line callback. Toggling DTR/RTS here
//                         is what puts the chip into download mode, so this is
//                         the port a flasher talks to.
//   kCdcItf   (port 1) -- the data/protocol port used by the host plugin. It has
//                         NO line-state callback, and, more importantly, it is a
//                         different /dev node, so the plugin holding it open does
//                         not lock the flasher out.
//
// Why the split matters, both reasons learned the hard way:
//   1. Opening a macOS serial node asserts DTR/RTS. On the flashing port those
//      lines are the reset signal, so a host program that merely opens the port
//      to "check whether the board is there" kicks the chip out of ROM download
//      mode and makes flashing impossible.
//   2. Even with that fixed, a permanently open port is an exclusively held
//      port: esptool gets "Serial data stream stopped" and cannot flash at all.
// Two ports solve both.
constexpr tinyusb_cdcacm_itf_t kFlashItf = TINYUSB_CDC_ACM_0;
constexpr tinyusb_cdcacm_itf_t kCdcItf = TINYUSB_CDC_ACM_1;

// ---------------------------------------------------------------------------
// Copied directly from the USB descriptors of the ROM download-mode device
// (measured with ioreg)
//
//   idVendor 0x303A  idProduct 0x0020  bcdUSB 0x0200  bcdDevice 0x0764
//   bDeviceClass 0xEF / SubClass 0x02 / Protocol 0x01  bMaxPacketSize0 64
//   strings 1="Espressif" 2="ESP32-S31" 3="1c:29:04:d0:97:d8" (MAC)
//
// The hyphen in the product name was determined by measurement: the ROM device is
// called "ESP32-S31" in the macOS system log, while the application firmware was
// initially written as "ESP32_S31" (underscore), and the log makes it obvious at a
// glance that they are two different devices -- which breaks the premise that "the
// host sees the same device before and after reset".
//
// The PID is the chip ID, so the host sees the same device in application state
// and in ROM state.
// ---------------------------------------------------------------------------
constexpr uint16_t kEspressifVid = 0x303A;
constexpr uint16_t kChipIdPid = 0x0020;

const tusb_desc_device_t kDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,        // 0xEF
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,  // 0x02
    .bDeviceProtocol = MISC_PROTOCOL_IAD,     // 0x01
    .bMaxPacketSize0 = 64,
    .idVendor = kEspressifVid,
    .idProduct = kChipIdPid,
    .bcdDevice = 0x0764,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// "1c:29:04:d0:97:d8" -- generated at runtime from the chip MAC, identical to the
// ROM.
char g_serial[18];

// The type of tinyusb_desc_config_t::string is const char**, so this cannot be
// written as const char* const[] (that would be an array of const pointers to
// const, which does not convert).
const char* kStringDescriptors[] = {
    (const char[]){0x09, 0x04},  // 0: language ID (English)
    "Espressif",                 // 1: manufacturer
    "ESP32-S31",                 // 2: product (note the hyphen, not an underscore)
    g_serial,                    // 3: serial number = MAC
    "FluidBox CDC",              // 4: CDC interface name
};
constexpr int kStringDescriptorCount =
    sizeof(kStringDescriptors) / sizeof(kStringDescriptors[0]);

// ---------------------------------------------------------------------------
// Button-free flashing state
// ---------------------------------------------------------------------------
volatile bool g_reboot_to_bootloader = false;
volatile bool g_reboot_pending = false;

// Callback invoked before restarting into ROM download mode (registered in main,
// used to turn the screen off first and save power).
PreRestartHook g_pre_restart_hook = nullptr;

// Received bytes go in here first, and the caller takes them out to parse.
//
// **64KB** -- it must hold one band's payload (480x30x2 = 28800 bytes). In
// addition, when playing audio the PC pushes PCM at the playback rate and the
// read-ahead has to fit (measured: about 16KB ahead for 4 seconds of audio).
// It started at 4KB, and the result was that relaying a single band inevitably
// overflowed midway; at the time the "break when full" code stopped the callback
// from consuming the USB FIFO -> FIFO full -> CDC endpoint NAKs -> **the PC-side
// write blocks forever**. The symptom was "the PC sends 28KB and both sides freeze
// together".
osal::RingBuffer* g_rx = nullptr;
volatile uint32_t g_rx_overflow = 0;

// Whether the "arm" step of the esptool signature has been seen.
bool g_saw_esptool_arm = false;

esp_timer_handle_t g_restart_timer = nullptr;

void RestartTimerCallback(void* arg) {
    (void)arg;
    // Turn off the power-hungry things before restarting, to avoid brownout during
    // flashing.
    if (g_pre_restart_hook != nullptr) {
        g_pre_restart_hook();
    }
    if (g_reboot_to_bootloader) {
        // The S31 register name differs from the S3:
        //   S3      -> RTC_CNTL_FORCE_DOWNLOAD_BOOT
        //   C6/C5   -> LP_AON_FORCE_DOWNLOAD_BOOT
        //   S31     -> LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT (SYS_CTRL bit 2)
        REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    }
    esp_restart();
}

// ---------------------------------------------------------------------------
// Button-free entry into download mode: **the full two-step esptool signature must
// be recognised, a falling edge alone is not enough**
// ---------------------------------------------------------------------------
// esptool's signature for entering download mode has two steps:
//
//     1) arm:    RTS high, DTR low      (dtr=0, rts=1)
//     2) swap:   RTS low, DTR high      (dtr=1, rts=0)   <- this step triggers
//
// **Why "RTS falling edge + DTR high" alone is not enough**: that was the old
// S2/S3 behaviour, and its premise is that the ROM can keep USB alive across a
// reset. **In the S31 rev0 ROM, `chip_usb_dw_prepare_persist` is an empty stub, so
// USB does not survive a reset** -- which means any ordinary serial port
// open/close that produces a single RTS falling edge kicks the chip into download
// mode and then the port drops off. The symptom was "every time I finish flashing
// and go read the log, the device is gone".
//
// This is exactly how the official code tightened it on the S31, see esp-idf
// issue #18992:
//   "reboot-to-bootloader triggers only on the esptool entry signature
//    (RTS asserted with DTR clear, then swapped); ordinary terminal
//    open/close line-state edges no longer reboot the chip."
void LineStateChangedCallback(int itf, cdcacm_event_t* event) {
    (void)itf;
    const bool dtr = event->line_state_changed_data.dtr;
    const bool rts = event->line_state_changed_data.rts;

    if (rts && !dtr) {
        // Step 1: arm.
        g_saw_esptool_arm = true;
        return;
    }

    if (!rts && dtr) {
        // Step 2: swap. It only counts if the arm step was seen first.
        if (g_saw_esptool_arm && !g_reboot_pending) {
            g_reboot_to_bootloader = true;
            g_reboot_pending = true;
            // Wait a moment before restarting, so data in the CDC can drain first.
            esp_timer_start_once(g_restart_timer, 100 * 1000);
        }
        g_saw_esptool_arm = false;
        return;
    }

    // The other combinations (the (1,1) intermediate state, (0,0) idle) **do not
    // clear the arm flag** -- in esptool's sequence, (0,1) passes through (1,1)
    // before reaching (1,0), and clearing on the intermediate state would make it
    // impossible to ever trigger.
}


// TinyUSB's RX callback. **It runs in the TinyUSB task context and must never
// block.**
//
// There are three pitfalls here, all learned by measurement (the symptom being: the
// PC sends one 28KB band and both sides freeze together):
//
// 1. **When the ring buffer is full, the USB read must absolutely not `break`.**
//    The first version broke out when full, and the result was that the callback
//    stopped consuming the USB FIFO -> the FIFO filled up too -> the CDC endpoint
//    started NAKing -> **the PC-side write blocked forever**. The symptom is "it
//    is stuck".
//    The right approach: when full, **drop and count**, but keep draining the FIFO
//    -- USB must always be consumed.
//
// 2. **Drain the FIFO as much as possible on every callback.**
//    There is a counter-intuitive point here: the first version capped it at
//    "only 8 blocks per call", thinking "do not hog the CPU". But then **the
//    remaining data might never trigger another callback** (TinyUSB's
//    CDC_EVENT_RX fires when there is new data, and the part already sitting in the
//    FIFO but not yet read is not necessarily reported again) -- which turned into
//    a freeze again.
//    In practice, draining a full 32KB is only 225 rounds of memcpy, on the order
//    of microseconds, so it is not a CPU burden at all.
//    The cap of 256 blocks (32KB) is there as an escape hatch in case the peer goes
//    haywire; normally one pass drains it completely.
//
// 3. **The ring buffer must hold one band.** One band's payload is
//    480x30x2 = 28800 bytes; if the buffer were smaller, the callback would
//    inevitably overflow halfway through. 32KB covers one full band plus margin.
constexpr size_t kRxChunkMax = 256;   // how much to read from the FIFO at a time
constexpr int kRxDrainPerCallback = 256;  // cap 256x128B = 32KB; normally drained in one pass

void RxCallback(int itf, cdcacm_event_t* event) {
    (void)itf;
    (void)event;
    if (g_rx == nullptr) {
        return;
    }

    uint8_t buf[kRxChunkMax];
    for (int i = 0; i < kRxDrainPerCallback; i++) {
        size_t len = 0;
        if (tinyusb_cdcacm_read(kCdcItf, buf, sizeof(buf), &len) != ESP_OK || len == 0) {
            return;  // FIFO empty, done
        }
        if (!g_rx->Send(osal::InputBuffer(buf, len), 0)) {
            // **Drop, but keep reading.** See point 1 above -- breaking here would
            // freeze the PC.
            g_rx_overflow = g_rx_overflow + 1;
        }
    }
}

int CdcVprintf(const char* fmt, va_list ap) {
    char buf[256];
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0 && tinyusb_cdcacm_initialized(kCdcItf)) {
        const size_t len =
            (n < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(n)
                                                : sizeof(buf) - 1;
        tinyusb_cdcacm_write_queue(kCdcItf,
                                   reinterpret_cast<const uint8_t*>(buf), len);
        tinyusb_cdcacm_write_flush(kCdcItf, 0);
    }
    return n;
}

// Fill in a serial number string identical to the ROM's, using the chip MAC.
void BuildSerialString() {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    snprintf(g_serial, sizeof(g_serial), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace

esp_err_t Init() {
    BuildSerialString();

    // 1. TinyUSB device stack. The S31 is a single-port high-speed chip, and port 0
    // is itself the high-speed port.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &kDeviceDescriptor;
    tusb_cfg.descriptor.string = kStringDescriptors;
    tusb_cfg.descriptor.string_count = kStringDescriptorCount;
    const esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        return err;
    }

    // 2a. Flashing port: control-line callback only, no RX.
    //
    // This is the core of button-free flashing and it stays on port 0. The
    // plugin never opens this port, so nothing the host does for ordinary
    // display traffic can reset the chip.
    const tinyusb_config_cdcacm_t flash_cfg = {
        .cdc_port = kFlashItf,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = LineStateChangedCallback,
        .callback_line_coding_changed = nullptr,
    };
    const esp_err_t flash_err = tinyusb_cdcacm_init(&flash_cfg);
    if (flash_err != ESP_OK) {
        return flash_err;
    }

    // 2b. Data port: RX callback, no control-line callback.
    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = kCdcItf,
        .callback_rx = RxCallback,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = nullptr,
        .callback_line_coding_changed = nullptr,
    };
    const esp_err_t acm_err = tinyusb_cdcacm_init(&acm_cfg);
    if (acm_err != ESP_OK) {
        return acm_err;
    }

    // 2.5 Receive ring buffer; the RX callback was already registered above.
    if (g_rx == nullptr) {
        g_rx = new osal::RingBuffer(65536);
    }

    // 3. Restart timer.
    const esp_timer_create_args_t timer_args = {
        .callback = RestartTimerCallback,
        .arg = nullptr,
        .name = "usb_boot",
        .skip_unhandled_events = false,
    };
    const esp_err_t timer_err = esp_timer_create(&timer_args, &g_restart_timer);
    if (timer_err != ESP_OK) {
        return timer_err;
    }

    // 4. Register with the VFS so it can be opened as an ordinary serial port.
    const esp_err_t vfs_err = esp_vfs_tusb_cdc_register(kCdcItf, nullptr);
    if (vfs_err != ESP_OK) {
        return vfs_err;
    }

    ESP_LOGI(kTag, "USB-OTG CDC ready (303A:%04X, SN=%s), button-free flashing armed",
             static_cast<unsigned>(kChipIdPid), g_serial);
    return ESP_OK;
}

bool Connected() {
    return tinyusb_cdcacm_initialized(kCdcItf);
}

size_t Write(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0 || !tinyusb_cdcacm_initialized(kCdcItf)) {
        return 0;
    }
    const size_t queued = tinyusb_cdcacm_write_queue(kCdcItf, data, len);
    tinyusb_cdcacm_write_flush(kCdcItf, 0);
    return queued;
}

size_t Write(const char* text) {
    return (text == nullptr) ? 0 : Write(reinterpret_cast<const uint8_t*>(text),
                                         strlen(text));
}

void Printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t len =
            (n < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(n)
                                                : sizeof(buf) - 1;
        Write(reinterpret_cast<const uint8_t*>(buf), len);
    }
}

void SetPreRestartHook(PreRestartHook hook) { g_pre_restart_hook = hook; }

void AttachToLogger() {
    esp_log_set_vprintf(CdcVprintf);
}

osal::InputBuffer ReceiveRx(uint32_t timeout_ms) {
    if (g_rx == nullptr) {
        return {};
    }
    return g_rx->Receive(timeout_ms);
}

void ReturnRx(osal::InputBuffer buf) {
    if (g_rx != nullptr && !buf.empty()) {
        g_rx->ReturnItem(buf);
    }
}

uint32_t RxOverflowCount() { return g_rx_overflow; }

void RebootToBootloader() {
    g_reboot_to_bootloader = true;
    REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

}  // namespace usb_console
