// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// usb_console -- USB-OTG CDC console + button-free flashing
//
// Why not USB Serial/JTAG
// -----------------------
// The ESP32-S31 has two USB peripherals and they cannot work at the same time
// (Espressif's official FAQ):
//   - USB-OTG          <- this is what the ESP-Mosaico Type-C is wired to
//   - USB Serial/JTAG  <- routed to pins 13/15 of the H2 module header on the left
// ROM download mode also goes through OTG (esptool identifies it as
// "USB mode: USB-OTG", VID 0x303A / PID = chip ID). So to make the virtual serial
// port on the Type-C support button-free flashing, the application layer has to
// emulate what the ROM CDC does.
//
// How button-free flashing works
// ------------------------------
// When esptool resets the chip it drives this sequence on the CDC control lines:
//
//     DTR=0, RTS=1  ->  DTR=1, RTS=0  ->  DTR=0, RTS=0
//                        ^^^^^^^^^^^^
//                        key point: DTR is high on the RTS falling edge
//
// We catch that edge, set the S31's LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT and reboot;
// the ROM comes up straight into download mode -- no BOOT button press anywhere.
//
// The descriptors must match the ROM
// ----------------------------------
// The application state and the ROM download state use the same
// VID/PID/strings, so macOS assigns the same /dev/cu.usbmodemXXXX and the flashing
// script can obtain the same port name before and after the reset.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "osal.h"

namespace usb_console {

// Install TinyUSB + CDC-ACM, register the VFS, and hook up the control-line
// callback. After this returns, Printf() can be used, or /dev/tusb_cdc can be
// used as an ordinary serial port.
esp_err_t Init();

// Whether the host has opened the CDC.
bool Connected();

// Write straight to the CDC.
//
// **No mutex protection here**: the underlying tinyusb_cdcacm_write_queue /
// tud_cdc_n_write take no locks. Multiple tasks calling Write()/Printf() at the
// same time will interleave. Today the only callers are the main task and the
// statistics log, so in practice there is no concurrency; if this is to be used
// from multiple tasks in the future, an osal::Mutex must be added here first.
size_t Write(const uint8_t* data, size_t len);
size_t Write(const char* text);

// printf-style output.
void Printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Redirect ESP-IDF logs to the CDC.
void AttachToLogger();

// ---- Receive ----------------------------------------------------------------
//
// **This layer knows nothing about the protocol**, it just hands over the bytes it
// receives. Protocol parsing (frame headers, JSON lines) happens in each caller
// -- that keeps usb_console generic, so a future protocol change does not require
// touching it.
//
// Prerequisite: Init() has registered the CDC RX callback, and received data goes
// into a ring buffer.

// Take a chunk of received data. **An empty view means timeout** (not an error).
//
// The returned view points into the ring buffer's internal storage, and **it must
// be given back with ReturnRx() when done** -- a FreeRTOS ring buffer in no-split
// mode has "lend" semantics, and failing to return it fills the buffer quickly.
osal::InputBuffer ReceiveRx(uint32_t timeout_ms = UINT32_MAX);

// Return the chunk lent out by ReceiveRx().
void ReturnRx(osal::InputBuffer buf);

// How many times the ring buffer has been full (meaning consumption is too slow or
// the peer is sending too fast). Diagnostic use.
uint32_t RxOverflowCount();

// Callback invoked before restarting into ROM download mode.
//
// **Why it is needed**: GPIO state is lost after a reboot, and the ROM does not
// drive the screen's 3.3V control pin. If the AMOLED were still lit at that point,
// the entire flashing process would draw extra power -- and writing flash already
// has current spikes, so the combination can collapse the supply, showing up as
// "the port disappears halfway through flashing".
// Registering a hook that turns the screen off eliminates that variable.
using PreRestartHook = void (*)();
void SetPreRestartHook(PreRestartHook hook);

// Immediately reboot into ROM download mode (equivalent to esptool's handshake).
// In the normal flow this is triggered automatically by the control-line callback;
// this interface is for manual commands.
void RebootToBootloader();

}  // namespace usb_console
