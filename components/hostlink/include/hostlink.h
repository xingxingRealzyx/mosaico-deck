// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// hostlink.h -- PC <-> board USB link
//
// Why the name is not `link`
// --------------------------
// **`link` is a POSIX system call name** (`int link(const char*, const char*)` in unistd.h).
// Using it as a namespace means that as soon as some header indirectly pulls in
// unistd.h you get "redeclared as different kind of entity" -- and the error is
// reported inside a system header, so the cause is very hard to spot.
// This project actually hit that once.
//
// Protocol
// --------
// PC -> board: **binary frames** (band pixels are 28800 bytes, which does not fit in JSON)
//
//     +--------+------+-----------+--------------+
//     | magic  | type | len (4,LE)| payload      |
//     | DE AD  |  1B  |           | len bytes    |
//     +--------+------+-----------+--------------+
//
//     type 0x01 = one band: payload = idx(1B) + 480x30x2 pixels (RGB565, byte order already fixed)
//     type 0x02 = full screen:    payload = 480x480x2 pixels
//     type 0x10 = control:    payload = one line of JSON (UTF-8, no newline)
//     type 0x20 = playback:    payload = interleaved stereo 16-bit PCM (48kHz)
//     type 0x21 = recording control: payload = one line of JSON ({"cmd":"start"|"stop"})
//
// board -> PC: **JSON lines** (all messages are small; readability matters more than
// saving a few bytes)
//
//     {"t":"hello","fw":"...","bands":16}
//     {"t":"touch","ev":"down","x":123,"y":456}
//     {"t":"touch","ev":"up"}
//     {"t":"key","ev":"short"|"long"}
//     {"t":"ack","band":3}
//     {"t":"err","msg":"..."}
//     {"t":"audio","bytes":N}\n followed by N bytes of PCM -- **recording goes this way**
//
// Why recording is "a JSON line reporting the length first, then raw data" instead of
// a pure binary frame:
// In the board-to-PC direction everything is normally JSON lines, and reading line by
// line is the simplest thing on the PC side. Only audio needs to carry binary, so we
// glue the two forms together in the most minimal way: "a length-first line + raw data"
// -- the PC side reads one line, then reads N bytes according to the length; there is
// no need to re-find a sync point in the stream.
//
// Why the board side **needs no frame buffer**
// --------------------------------------------
// As soon as one band arrives it is written straight into that band's buffer and pushed
// out. Therefore:
//   - Partial updates are naturally supported -- the PC only sends the bands that changed,
//     and the panels that were not sent stay exactly as they were
//   - The board does not need to know what the UI looks like; it is a dumb terminal
//   - The 28KB payload is received into the band buffer **as a stream**, without going
//     through an intermediate buffer

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace hostlink {

// Inject the panel to drive. **Must be called before Init()** -- link does not know
// where the panel comes from; main decides (so link does not depend on how display is
// concretely constructed either).
// Passing nullptr is legal: when the panel is not ready, link keeps receiving frames and
// simply does not display them (otherwise the stream would get out of sync).
void SetPanel(void* panel);

// Create the RX task and hook up input reporting. The USB console must have been
// Init()'d first.
esp_err_t Init();

// Start/stop pushing microphone data to the PC.
//
// Driven by the AI button (hold to record, release to stop) -- this is the most natural
// interaction: speak whenever you want, no need to hunt for a button on the screen.
// The PCM is 48kHz interleaved stereo 16-bit, about 192KB/s, which USB handles easily.
void StartCapture();
void StopCapture();
bool Capturing();

// Report input. Called by the touch/key tasks -- it locks internally, so multiple tasks
// may call it concurrently.
void SendTouch(bool down, int x, int y);
void SendKey(const char* ev);

// Whether the PC has ever sent a frame.
//
// **The screen can only have one owner.** If the local self-test pattern and the frames
// sent by the PC are drawn at the same time, the two tasks fight over the SPI bus
// (measured: `acquire spi bus failed` + `ESP_ERR_INVALID_STATE`), and once the consumer
// task is stuck on the bus it has no time left to read the ring buffer -- which shows up
// as heavy RX overflow.
//
// So the render task in main uses this to decide: once the PC takes over, the local
// pattern stops.
bool RemoteOwnsScreen();

// How much was received / dropped. For diagnostics.
struct Stats {
    uint32_t frames = 0;    // number of frames parsed successfully
    uint32_t bands = 0;     // number of bands received
    uint32_t full = 0;      // number of full-screen frames received
    uint32_t ctrl = 0;      // number of control messages received
    uint32_t bad_magic = 0;  // frame header mismatch (usually means bytes were lost earlier)
    uint32_t bad_len = 0;   // length out of range
    uint32_t rx_timeout = 0;  // timeout while receiving payload (the peer stopped sending)
    uint32_t audio_in = 0;    // number of playback frames received
    uint32_t audio_out = 0;   // number of recorded chunks pushed up
};
Stats GetStats();

}  // namespace hostlink
