// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// link.cpp -- PC <-> board USB link

#include "hostlink.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <atomic>
#include <cstdio>

#include "audio.h"
#include "display.h"
#include "esp_log.h"
#include "fbox/config.h"
#include "osal.h"
#include "usb_console.h"

namespace hostlink {
namespace {

constexpr const char* kTag = "hostlink";

// Magic number of the binary frame. Two bytes instead of one: once the stream is
// misaligned, a single-byte magic is easily hit by coincidence, while two bytes almost
// never are (1/65536) -- and it lets us realign after a slip.
constexpr uint8_t kMagic0 = 0xDE;
constexpr uint8_t kMagic1 = 0xAD;

constexpr uint8_t kTypeBand = 0x01;
constexpr uint8_t kTypeFull = 0x02;
constexpr uint8_t kTypeCtrl = 0x10;
constexpr uint8_t kTypeAudio = 0x20;
constexpr uint8_t kTypeRec = 0x21;

// Chunk size for playback/recording.
//
// During playback we receive exactly as much as the PC sends at a time, but **we must
// not receive one big block and then play it** -- that would let latency accumulate.
// 2KB at 48kHz stereo is 10.7ms, right between "no stutter" and "no choppiness".
//
// WARNING: **This buffer must be static; it cannot live on the stack.**
// It was originally written as `uint8_t buf[kAudioChunk]` (4096) and the link_rx task
// stack was also exactly 4096 -- entering HandleAudio blew the stack and reset. The
// symptom was "the board reboots as soon as audio is sent", with no readable error
// message at all (a stack overflow does not print a backtrace; it just resets).
// Embedded stacks are usually only a few KB, so any buffer larger than a few hundred
// bytes should not go on the stack.
constexpr size_t kAudioChunk = 2048;

// How much recording reads per push.
constexpr size_t kRecChunk = 4096;

constexpr size_t kBandPayload = 1 + fluidbox::config::kBandPixels * 2;      // idx + pixels
constexpr size_t kFullPayload = fluidbox::config::kLcdHRes * fluidbox::config::kLcdVRes * 2;

// Upper bound on the payload of a single audio frame: 1 second of 48kHz stereo = 192KB.
// Allow 256KB of headroom; anything larger means the protocol has gone wrong.
constexpr uint32_t kAudioPayloadMax = 256 * 1024;

std::atomic<bool> g_capturing{false};

display::Panel* g_panel = nullptr;
hostlink::CtrlSink g_ctrl_sink = nullptr;
Stats g_stats;

// Lock for sending messages. **Mandatory**: usb_console::Write has no internal lock (its
// own comment says so), and both the touch task and the key task send messages, so
// without a lock they would interleave.
osal::Mutex* g_tx_lock = nullptr;

// Upper bound on a control message payload. They are all small JSON; 512 bytes is plenty.
constexpr size_t kCtrlMax = 512;

// ---------------------------------------
// Byte stream reader
// ---------------------------------------
// The layer below hands us **variable-length chunks** (one RX callback is at most 128
// bytes), while the protocol wants "exactly N bytes". The leftover tail of a chunk must
// not be dropped -- it is the beginning of the next field. So we keep a carry.
//
// The carry only needs to hold one chunk (128 bytes); 256 is generous.
class RxStream {
 public:
    // Receive exactly len bytes. Returns the number of bytes actually received --
    // **less than len means a timeout**, and the caller must treat it as an error and
    // must not assume more data is coming.
    size_t Read(uint8_t* dst, size_t len, uint32_t timeout_ms) {
        size_t got = 0;

        // First spit out whatever was left over from last time
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
                break;  // timeout
            }
            const size_t need = len - got;
            const size_t take = (chunk.size() < need) ? chunk.size() : need;
            memcpy(dst + got, chunk.data(), take);
            got += take;

            // Store the leftover tail of the chunk in carry; use it first next time
            if (take < chunk.size()) {
                const size_t rest = chunk.size() - take;
                if (rest <= sizeof(carry_) - carry_len_) {
                    memcpy(carry_ + carry_len_, chunk.data() + take, rest);
                    carry_len_ += rest;
                }
                // If carry cannot hold it the protocol has gone wrong (a single chunk is
                // at most 128 bytes); drop it and count it
            }
            usb_console::ReturnRx(chunk);
        }
        return got;
    }

    // Is another message already waiting? **Non-blocking**, and whatever comes back is
    // kept in carry so the normal read path finds it again.
    //
    // This is how the band path knows a batch has ended without the PC having to say so:
    // a batch arrives back to back, so "nothing pending" means the update is complete.
    bool HasPending() {
        if (carry_len_ > 0) {
            return true;
        }
        const osal::InputBuffer chunk = usb_console::ReceiveRx(0);
        if (chunk.empty()) {
            return false;
        }
        const size_t take = (chunk.size() < sizeof(carry_)) ? chunk.size() : sizeof(carry_);
        memcpy(carry_, chunk.data(), take);
        carry_len_ = take;
        usb_console::ReturnRx(chunk);
        return true;
    }

    // Drop everything in carry. Called when the frame header does not match --
    // re-finding the sync point is safer than continuing to guess.
    void Resync() { carry_len_ = 0; }

 private:
    uint8_t carry_[256] = {};
    size_t carry_len_ = 0;
};

RxStream g_rx;

// ---------------------------------------
// board -> PC
// ---------------------------------------
void SendLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Internal implementation of sending one line. **The caller must already hold g_tx_lock.**
size_t SendLineLocked(const char* fmt, va_list ap) {
    char buf[192];
    const int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    if (n <= 0) {
        return 0;
    }
    size_t len = (static_cast<size_t>(n) < sizeof(buf) - 2) ? static_cast<size_t>(n)
                                                            : sizeof(buf) - 2;
    buf[len++] = '\n';
    usb_console::Write(reinterpret_cast<const uint8_t*>(buf), len);
    return len;
}

void SendLine(const char* fmt, ...) {
    osal::LockGuard<osal::Mutex> guard(*g_tx_lock);
    va_list ap;
    va_start(ap, fmt);
    SendLineLocked(fmt, ap);
    va_end(ap);
}

// **One line of JSON immediately followed by raw binary must be sent under the same lock.**
//
// Otherwise a log line would be inserted in the middle -- the PC side parses by "read one
// line, then read N bytes", and one inserted log line scrambles everything.
// (At first we wanted to pass the data in through the variadic arguments too, but a
//  va_list cannot yield a binary pointer, so it was split into this dedicated function,
//  with the header and the payload as one unit.)
void SendAudioUp(const void* data, size_t len) {
    osal::LockGuard<osal::Mutex> guard(*g_tx_lock);
    char head[96];
    const int n = snprintf(head, sizeof(head), "{\"t\":\"audio\",\"bytes\":%u}\n",
                           static_cast<unsigned>(len));
    if (n > 0) {
        usb_console::Write(reinterpret_cast<const uint8_t*>(head),
                           static_cast<size_t>(n));
    }
    usb_console::Write(static_cast<const uint8_t*>(data), len);
}

namespace {

// Pull one value out of our own three-key control JSON, e.g.
// {"cmd":"volume","value":70}.
//
// **Deliberately not a JSON parser.** The payload is written by the PC side of this same
// repository, the shape is fixed, and a real parser would be a dependency for two
// fields. What matters instead is that malformed input cannot misbehave: every scan is
// bounded, so a truncated or hostile frame just fails to parse.
// Set by the receive path, drained by CommitTask. **Never do the commit inline**: it
// waits for a TE edge (up to 20ms) and then occupies the bus for ~12ms, and the receive
// task is the only thing draining USB. Blocking it there overflowed the receive ring
// (measured: RX overflow 1743 during one full frame) and dropped bands, which on screen
// is a picture with pieces missing.
std::atomic<bool> g_commit_requested{false};

// When the last band was staged. File scope because the band handler and the receive
// loop are separate functions; both run on the same task, so no locking is needed.
int64_t g_last_band_us = 0;

// How long the stream has to be quiet before the fallback commit fires.
//
// **It has to be generous.** The PC converts each band's 14400 pixels to RGB565 in
// JavaScript before writing it, so the USB stream genuinely goes quiet for milliseconds
// in the *middle* of a batch. Committing on the first quiet moment -- which an earlier
// version did -- pushed the update in pieces, which is exactly the strip-by-strip look
// the staging buffer exists to remove. The explicit `commit` command is the real path;
// this only rescues a PC side that never sends one.
constexpr int64_t kBatchIdleUs = 80 * 1000;

bool FindString(const char* text, const char* key, char* out, size_t out_size) {
    const char* at = strstr(text, key);
    if (at == nullptr) {
        return false;
    }
    at = strchr(at + strlen(key), ':');
    if (at == nullptr) {
        return false;
    }
    while (*++at == ' ' || *at == '\t') {
    }
    if (*at != '"') {
        return false;
    }
    ++at;
    size_t i = 0;
    while (*at != '\0' && *at != '"' && i + 1 < out_size) {
        out[i++] = *at++;
    }
    out[i] = '\0';
    return *at == '"';
}

bool FindInt(const char* text, const char* key, int* out) {
    const char* at = strstr(text, key);
    if (at == nullptr) {
        return false;
    }
    at = strchr(at + strlen(key), ':');
    if (at == nullptr) {
        return false;
    }
    while (*++at == ' ' || *at == '\t') {
    }
    if (*at < '0' || *at > '9') {
        return false;              // these values are never negative
    }
    int value = 0;
    while (*at >= '0' && *at <= '9') {
        value = value * 10 + (*at - '0');
        if (value > 1000000) {
            return false;          // bounded: no overflow, no runaway scan
        }
        ++at;
    }
    *out = value;
    return true;
}

}  // namespace

void HandleCtrl(const uint8_t* payload, size_t len) {
    // The payload does not necessarily end with '\0' (the protocol carries no
    // terminator), so copy it into a stack buffer first.
    char text[kCtrlMax + 1];
    const size_t n = (len < kCtrlMax) ? len : kCtrlMax;
    memcpy(text, payload, n);
    text[n] = '\0';

    char cmd[16];
    int value = 0;
    if (!FindString(text, "\"cmd\"", cmd, sizeof(cmd)) || !FindInt(text, "\"value\"", &value)) {
        ESP_LOGW(kTag, "control message without cmd/value: %s", text);
        SendLine("{\"t\":\"err\",\"msg\":\"bad control payload\"}");
        return;
    }
    if (g_ctrl_sink == nullptr || !g_ctrl_sink(cmd, value)) {
        ESP_LOGW(kTag, "control command not handled: %s=%d", cmd, value);
        SendLine("{\"t\":\"err\",\"msg\":\"unknown control command\"}");
        return;
    }
    ESP_LOGI(kTag, "control %s=%d", cmd, value);
    // Echo the command back: the PC side uses this to confirm the board actually applied
    // it rather than merely receiving the frame.
    SendLine("{\"t\":\"ack\",\"ctrl\":\"%s\",\"value\":%d}", cmd, value);
}

// Receive one band's pixels **as a stream** into the band buffer.
//
// This is the most critical piece in this component: the 28800 bytes are not received
// into an intermediate buffer and then copied over; they are moved directly between the
// ring buffer and the band buffer -- saving 28KB of resident memory, on a chip that has
// only 512KB of SRAM in total.
void HandleBand(bool full_frame) {
    const size_t total = full_frame ? kFullPayload : kBandPayload;

    // For a single band, read the idx first
    uint8_t idx = 0;
    if (!full_frame) {
        if (g_rx.Read(&idx, 1, 1000) != 1) {
            g_stats.rx_timeout = g_stats.rx_timeout + 1;
            return;
        }
        if (idx >= fluidbox::config::kBandCount) {
            g_stats.bad_len = g_stats.bad_len + 1;
            // The content still has to be drained, otherwise everything after it is
            // misaligned -- but since the length is known, read it into a temporary area
            // and discard it. A small discard loop is used here; no memory is allocated.
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
        // Even if the panel is not ready the data must be drained, otherwise the stream
        // gets misaligned
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

    // From here to the end of the function, the display is held exclusively.
    //
    // At the transitional instant it can still collide with the local self-test pattern
    // (the PC's frame has just arrived and the render task has not yet seen the yield
    // flag), and that moment reports `acquire spi bus failed`. This lock is what catches
    // it.
    osal::LockGuard<osal::Mutex> draw_guard(g_panel->DrawMutex());

    if (full_frame) {
        // Full screen: receive band by band, each 480x30x2 bytes, stage them all, then
        // push **one** transfer. Staging is what makes the update look like a new picture
        // rather than a wipe.
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
            g_panel->StageBand(band, buf);
        }
        g_commit_requested.store(true);
        g_last_band_us = 0;
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
    // Staged, not pushed. It goes on the glass as one transfer once this update's bands
    // have all arrived -- either because the PC said so (the `commit` control command) or
    // because nothing else is in flight, which is what "the batch ended" means when the
    // bands arrive back to back.
    g_panel->StageBand(idx, buf);
    g_stats.bands = g_stats.bands + 1;
    g_last_band_us = osal::TimeUs();

    // Send one ack every 64 bands so the PC side can confirm the link is alive --
    // answering every one would be too noisy.
    if ((g_stats.bands % 64) == 0) {
        SendLine("{\"t\":\"ack\",\"band\":%u}", static_cast<unsigned>(g_stats.bands));
    }
}

// Receive one block, play one block -- **no whole-segment buffering**.
//
// A single speak can be several hundred KB (5 seconds of 48kHz stereo is almost 1MB);
// receiving it all before playing is both slow and memory-hungry. Receiving and playing
// at the same time means only a few tens of milliseconds from the PC starting to send to
// sound coming out.
void HandleAudio(uint32_t len) {
    if (!audio::ready()) {
        // Even if audio is not up the data must be drained, otherwise the stream gets
        // misaligned
        uint8_t sink[256];
        uint32_t left = len;
        while (left > 0) {
            const size_t n = (left < sizeof(sink)) ? left : sizeof(sink);
            if (g_rx.Read(sink, n, 1000) != n) {
                g_stats.rx_timeout = g_stats.rx_timeout + 1;
                return;
            }
            left -= static_cast<uint32_t>(n);
        }
        return;
    }

    // static: see the note on kAudioChunk above. There is only one caller (the link_rx
    // task), so no locking is needed.
    static uint8_t buf[kAudioChunk];
    uint32_t left = len;
    while (left > 0) {
        const size_t n = (left < sizeof(buf)) ? left : sizeof(buf);
        if (g_rx.Read(buf, n, 1000) != n) {
            g_stats.rx_timeout = g_stats.rx_timeout + 1;
            return;
        }
        audio::Play(buf, n, 1000);
        left -= static_cast<uint32_t>(n);
    }
    g_stats.audio_in = g_stats.audio_in + 1;
}

// Recording: read the microphone -> push to the PC.
//
// Driven by the AI button (hold to record, release to stop). The rate is 48kHz stereo =
// 192KB/s, while USB full speed is 1.5MB/s, so bandwidth is more than sufficient.
void CaptureTask(void* arg) {
    (void)arg;
    static uint8_t buf[kRecChunk];

    for (;;) {
        if (!g_capturing.load()) {
            osal::Delay(10);
            continue;
        }
        const size_t got = audio::Record(buf, sizeof(buf), 100);
        if (got == 0) {
            osal::Delay(1);
            continue;
        }
        SendAudioUp(buf, got);
        g_stats.audio_out = g_stats.audio_out + 1;
    }
}

void CommitTask(void* arg) {
    (void)arg;
    for (;;) {
        if (!g_commit_requested.load()) {
            osal::Delay(2);
            continue;
        }
        g_commit_requested.store(false);
        if (g_panel == nullptr) {
            continue;
        }
        // **Wait for the panel's TE edge *before* taking the draw mutex.**
        //
        // The receive task takes the same mutex for every message it handles, and it is
        // the only thing draining USB. Waiting inside the lock -- up to 20 ms for TE plus
        // ~12 ms on the bus -- starved it for ~30 ms per commit, and a full-screen update
        // (16 bands, ~460 KB, which is what closing the panel is) arrives as a burst that
        // does not fit in the receive ring over that window. The result was measured as
        // `RX overflow55`: bands dropped, and because the PC diffs against what it has
        // already sent, the screen kept showing the old picture while the plugin had
        // already moved on. That is exactly "taps do nothing, and it only catches up when
        // a new turn repaints".
        //
        // With the wait moved out here, the lock covers only the queue calls.
        (void)g_panel->WaitForTeTick(20);
        osal::LockGuard<osal::Mutex> guard(g_panel->DrawMutex());
        g_panel->CommitFrame();
    }
}

void RxTask(void* arg) {
    (void)arg;
    ESP_LOGI(kTag, "link receive task started");

    // Find the frame header. **Misalignment must be tolerated**: as soon as one of the
    // reads above times out, a few bytes are missing from the stream and the frame
    // headers after it no longer match. Here we look for DE AD byte by byte and
    // resynchronize when we find it.
    uint8_t prev = 0;
    // **Tell the PC when data was lost.** See the note on `dropped` in the plugin.
    uint32_t seen_overflow = usb_console::RxOverflowCount();
    for (;;) {
        const uint32_t overflow_now = usb_console::RxOverflowCount();
        if (overflow_now != seen_overflow) {
            SendLine("{\"t\":\"dropped\",\"n\":%u}",
                     static_cast<unsigned>(overflow_now - seen_overflow));
            seen_overflow = overflow_now;
        }
        // Fallback commit; see kBatchIdleUs. Only reached when the PC did not say
        // `commit` itself.
        if (g_panel != nullptr && g_panel->HasStaged() && g_last_band_us != 0
            && osal::TimeUs() - g_last_band_us >= kBatchIdleUs) {
            g_commit_requested.store(true);
            g_last_band_us = 0;
        }
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

        // Length upper-bound check. Without it, one bad length would make the board wait
        // for several MB of data.
        const uint32_t max_len = (type == kTypeFull)    ? kFullPayload
                                 : (type == kTypeBand)  ? kBandPayload
                                 : (type == kTypeAudio) ? kAudioPayloadMax
                                 : (type == kTypeRec)   ? kCtrlMax
                                                        : kCtrlMax;
        if (len > max_len) {
            g_stats.bad_len = g_stats.bad_len + 1;
            ESP_LOGW(kTag, "frame length out of range type=0x%02X len=%u (limit %u)", type,
                     static_cast<unsigned>(len), static_cast<unsigned>(max_len));
            g_rx.Resync();
            continue;
        }

        g_stats.frames = g_stats.frames + 1;

        if (type == kTypeBand) {
            HandleBand(false);
        } else if (type == kTypeFull) {
            HandleBand(true);
        } else if (type == kTypeAudio) {
            HandleAudio(len);
        } else if (type == kTypeRec) {
            // Recording control: {"cmd":"start"} / {"cmd":"stop"}
            uint8_t payload[kCtrlMax];
            if (g_rx.Read(payload, len, 1000) == len) {
                payload[len < kCtrlMax ? len : kCtrlMax - 1] = '\0';
                const bool start = strstr(reinterpret_cast<char*>(payload), "start") != nullptr;
                g_capturing.store(start);
                ESP_LOGI(kTag, "recording %s", start ? "start" : "stop");
                SendLine("{\"t\":\"ack\",\"rec\":%s}", start ? "true" : "false");
            } else {
                g_stats.rx_timeout = g_stats.rx_timeout + 1;
                g_rx.Resync();
            }
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

void RequestCommit() { g_commit_requested.store(true); }

void SetCtrlSink(CtrlSink sink) {
    g_ctrl_sink = sink;
}

void SetPanel(void* panel) {
    g_panel = static_cast<display::Panel*>(panel);
}

esp_err_t Init() {
    if (g_tx_lock == nullptr) {
        g_tx_lock = new osal::Mutex();
    }

    // **The priority must be higher than the TinyUSB task (default 5).**
    //
    // A very subtle pitfall was hit here: TinyUSB's RX callback runs in its own task
    // (priority 5), while the receive task was originally only 4 -- so **the callback
    // preempted the consumer the whole time**: data kept pouring into the ring buffer
    // while the consumer never got a time slice, and the buffer filled up quickly.
    // Measured: 1706 overflows (about 218KB, half the data lost just like that).
    //
    // After raising it to 6, the moment data arrives: the TinyUSB task runs the callback
    // and puts the data in the ring buffer, and is immediately preempted by the
    // higher-priority receive task which takes it away at once -- the buffer never backs
    // up again.
    //
    // USB is not starved: the receive task blocks on the ring buffer's Receive and
    // voluntarily yields when there is no data.
    static osal::Task capture_task;
    capture_task.Create(CaptureTask, nullptr,
                        {.name = "link_cap", .stack_size = 4096, .priority = 5,
                         .core = osal::CoreAffinity::kCore1});

    // Give it 6144 of stack: this task walks all three paths, HandleAudio / HandleBand /
    // HandleCtrl, which are all heavier than "just receive one line of JSON". Previously
    // 4096 paired with a 4096 stack buffer blew up directly.
    // The commit lives on its own task so the receive path never blocks on the panel:
    // it only raises the flag. Priority below rx (6) and above idle.
    static osal::Task commit_task;
    commit_task.Create(CommitTask, nullptr,
                       {.name = "link_commit", .stack_size = 4096, .priority = 4,
                        .core = osal::CoreAffinity::kCore1});

    static osal::Task rx_task;
    rx_task.Create(RxTask, nullptr,
                   {.name = "link_rx", .stack_size = 6144, .priority = 6,
                    .core = osal::CoreAffinity::kCore1});

    ESP_LOGI(kTag, "link started");
    return ESP_OK;
}

void StartCapture() { g_capturing.store(true); }
void StopCapture() { g_capturing.store(false); }
bool Capturing() { return g_capturing.load(); }

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

void SendTouchMove(int x, int y) {
    SendLine("{\"t\":\"touch\",\"ev\":\"move\",\"x\":%d,\"y\":%d}", x, y);
}

void SendKey(const char* ev) {
    if (g_tx_lock == nullptr) {
        return;
    }
    SendLine("{\"t\":\"key\",\"ev\":\"%s\"}", ev);
}

bool RemoteOwnsScreen() {
    // Yield as soon as the first frame header arrives; do not wait for a whole band to be
    // received.
    //
    // This was once written too late, and the consequence was that the whole chain
    // collapsed: the local pattern and the PC's frames fought over the SPI bus -> the
    // first band failed to queue -> bands stayed 0 -> this function kept returning false
    // -> the render task never yielded -> permanent contention -> the consumer task could
    // not read the ring buffer -> heavy RX overflow.
    // The log order is very telling: first a run of acquire spi bus failed, and only then
    // "PC took over the screen".
    return g_stats.frames > 0 || g_stats.bands > 0 || g_stats.full > 0;
}

Stats GetStats() { return g_stats; }

}  // namespace hostlink
