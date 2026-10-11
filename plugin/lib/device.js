// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// device.js -- talks to the mosaico-deck board (pure Node, no native modules)
//
// Why "pure Node"
// ---------------
// This plugin runs inside DeepSeek Harness, and **DSH is a signed app**, so any native
// Node module (serialport, sharp and the like) fails to load:
//
//     code signature not valid for use in process:
//     mapping process and mapped file (non-platform) have different Team IDs
//
// This is not a configuration problem, it is macOS library validation. So:
//
//   . **Serial port**: `stty` sets the tty to raw, then we read and write the device
//     file directly with `fs`. No serialport needed. The baud rate of USB CDC is
//     virtual anyway -- the device never looks at it. What actually matters is that
//     **the line discipline must be turned off** (the default canonical mode rewrites
//     binary data: newlines get converted, 0x11/0x13 get eaten as flow control).
//
//   . **Rendering**: handed to the Python that ships with DSH (3.12 + Pillow are both
//     in the runtime), see render.js.

import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { EventEmitter } from 'node:events';

export const H_RES = 480;
export const V_RES = 480;
export const BAND_ROWS = 30;
export const BAND_COUNT = V_RES / BAND_ROWS;
const BAND_PIXELS = H_RES * BAND_ROWS;

const MAGIC = Buffer.from([0xde, 0xad]);
const TYPE_BAND = 0x01;
const TYPE_AUDIO = 0x20;

const AUDIO_BYTES_PER_SEC = 48000 * 4; // 48kHz interleaved stereo 16-bit

/** Find a mosaico-deck serial port.
 *
 * Matching by name takes priority -- there may be other usbmodem devices plugged in.
 */
export function findPort() {
    let names = [];
    try {
        names = fs.readdirSync('/dev').filter((n) => n.startsWith('cu.usbmodem'));
    } catch {
        return null;
    }
    if (names.length === 0) return null;
    names.sort();
    return `/dev/${names[0]}`;
}

export class Deck extends EventEmitter {
    constructor(port = null) {
        super();
        this.port = port;
        this.fd = null;
        this._last = null; // previous frame's RGB, used for band diffing
        this._rx = Buffer.alloc(0);
        this._pendingAudio = 0;
        this._timer = null;
        this.stats = { bands: 0, bytes: 0, lastMs: 0, fulls: 0 };
    }

    get connected() {
        return this.fd !== null;
    }

    /** Open the serial port. Returns false on failure (does not throw) -- the board
     * not being plugged in is a perfectly normal state. */
    open() {
        if (this.fd !== null) return true;
        this.port = this.port || findPort();
        if (!this.port) return false;
        try {
            // raw must be set before opening: otherwise any data received in the
            // meantime has already been processed by the line discipline.
            execFileSync('stty', ['-f', this.port, 'raw', '-echo', '115200'], {
                stdio: 'ignore',
            });
            this.fd = fs.openSync(
                this.port,
                fs.constants.O_RDWR | fs.constants.O_NONBLOCK,
            );
        } catch {
            this.fd = null;
            return false;
        }
        this._last = null;
        this._rx = Buffer.alloc(0);
        this._pendingAudio = 0;
        this.emit('open', this.port);
        this._timer = setInterval(() => this._pump(), 8);
        return true;
    }

    close() {
        if (this._timer) clearInterval(this._timer);
        this._timer = null;
        if (this.fd !== null) {
            try {
                fs.closeSync(this.fd);
            } catch {
                /* already gone */
            }
            this.fd = null;
        }
        this.emit('close');
    }

    // ---- receive ----------------------------------------------------------
    _pump() {
        if (this.fd === null) return;
        const buf = Buffer.alloc(16384);
        for (;;) {
            let n = 0;
            try {
                n = fs.readSync(this.fd, buf, 0, buf.length, null);
            } catch (e) {
                if (e.code === 'EAGAIN') break;
                // The board was unplugged / reset. Close and let the caller reconnect.
                this.close();
                return;
            }
            if (n <= 0) break;
            this._rx = Buffer.concat([this._rx, buf.subarray(0, n)]);
        }
        this._parse();
    }

    _parse() {
        for (;;) {
            // A recording is "one line of JSON reporting the length + raw PCM", so we
            // first check whether there is pending raw data.
            // **It cannot be read as lines** -- PCM is full of \n.
            if (this._pendingAudio > 0) {
                if (this._rx.length < this._pendingAudio) return;
                const pcm = this._rx.subarray(0, this._pendingAudio);
                this._rx = this._rx.subarray(this._pendingAudio);
                this._pendingAudio = 0;
                this.emit('audio', pcm);
                continue;
            }
            const nl = this._rx.indexOf(0x0a);
            if (nl < 0) return;
            const line = this._rx.subarray(0, nl).toString('utf8').trim();
            this._rx = this._rx.subarray(nl + 1);
            if (!line.startsWith('{')) continue; // board log output, ignore
            let ev;
            try {
                ev = JSON.parse(line);
            } catch {
                continue;
            }
            if (ev.t === 'audio' && Number.isInteger(ev.bytes)) {
                this._pendingAudio = ev.bytes;
                continue;
            }
            this.emit('event', ev);
            this.emit(ev.t, ev);
        }
    }

    // ---- send -------------------------------------------------------------
    /** Write to the tty; **EAGAIN must be handled**.
     *
     * The file is opened with O_NONBLOCK, and writing a full 450KB frame in one call
     * exceeds the tty buffer, so writeSync throws EAGAIN. That is not an error, it
     * just means "cannot write right now, try later" -- back off and retry, otherwise
     * pushing a full frame crashes.
     */
    _writeAll(buf) {
        let off = 0;
        while (off < buf.length) {
            try {
                off += fs.writeSync(this.fd, buf, off, buf.length - off);
            } catch (e) {
                if (e.code === 'EAGAIN') {
                    // Sleep 1ms and retry. This cannot await (the caller is a synchronous
                    // render loop), so use Atomics.wait for a very short synchronous wait.
                    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1);
                    continue;
                }
                throw e;
            }
        }
    }

    _frame(type, payload) {
        const head = Buffer.alloc(7);
        MAGIC.copy(head, 0);
        head[2] = type;
        head.writeUInt32LE(payload.length, 3);
        this._writeAll(Buffer.concat([head, payload]));
    }

    /** Push RGB (480x480x3) up, **sending only the bands that changed**.
     *
     * This is the key to the whole design: a full 450KB frame takes 15.4ms over 60MHz
     * QSPI, while the panel refresh period is 17ms -- almost equal, so the slightest
     * jitter crosses a refresh and the picture tears. Sending only the changed bands
     * (one line of text is roughly 1 band = 8.7ms) lands safely inside a single refresh.
     */
    sendRgb(rgb, { force = false } = {}) {
        if (this.fd === null) return 0;
        const bytesPerBand = BAND_PIXELS * 3;

        const changed = [];
        for (let b = 0; b < BAND_COUNT; b++) {
            if (force || this._last === null) {
                changed.push(b);
                continue;
            }
            const off = b * bytesPerBand;
            if (!rgb.subarray(off, off + bytesPerBand).equals(
                this._last.subarray(off, off + bytesPerBand))) {
                changed.push(b);
            }
        }
        if (changed.length === 0) return 0;

        const t0 = Date.now();
        const payload = Buffer.alloc(1 + BAND_PIXELS * 2);
        for (const b of changed) {
            payload[0] = b;
            let j = 1;
            for (let i = b * BAND_PIXELS; i < (b + 1) * BAND_PIXELS; i++) {
                const r = rgb[i * 3], g = rgb[i * 3 + 1], bl = rgb[i * 3 + 2];
                // Standard RGB565, the high byte is R5G3 and the low byte is G3B5.
                const v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (bl >> 3);
                // WARNING: **the high byte must come first.**
                //
                // On the firmware side it is `buf[i] = swap16((r5 << 11) | b5)`, and buf is
                // a uint16_t on a little-endian CPU -- so the order that lands in memory is
                // [RGB565 high byte][low byte].
                //
                // This was written backwards at first (low byte first) and the symptom was
                // very misleading:
                //   . black 0x0000 is still black after the swap -> the background looks fine
                //   . white 0xFFFF is still white after the swap -> the main text looks fine
                //   . **all colors are wrong** -> accent-colored text, progress bars and
                //     status dots all get the wrong color
                // So it looked like "the font color is wrong".
                payload[j++] = (v >> 8) & 0xff;
                payload[j++] = v & 0xff;
            }
            this._frame(TYPE_BAND, payload);
        }
        this._last = Buffer.from(rgb);
        this.stats.bands += changed.length;
        this.stats.bytes += changed.length * BAND_PIXELS * 2;
        this.stats.lastMs = Date.now() - t0;
        if (changed.length === BAND_COUNT) this.stats.fulls++;
        return changed.length;
    }

    /** Push a block of 48kHz interleaved stereo 16-bit PCM for playback.
     *
     * **It must be sent at the playback rate.** The board plays while it receives, and
     * the playback speed is pinned at 192KB/s by the I2S DMA; dumping it all in at once
     * instantly fills its receive buffer (measured: only 5 of 99 chunks were received).
     * The 0.97 rate factor was measured: 4 seconds of audio runs only about 16KB ahead,
     * staying inside the 64KB buffer.
     */
    async speakPcm(pcm, { chunk = 2048 } = {}) {
        if (this.fd === null) throw new Error('board is not connected');
        const t0 = Date.now();
        let sent = 0;
        for (let i = 0; i < pcm.length; i += chunk) {
            const piece = pcm.subarray(i, i + chunk);
            this._frame(TYPE_AUDIO, piece);
            sent += piece.length;
            if (piece.length === chunk) {
                const target = t0 + (sent / AUDIO_BYTES_PER_SEC) * 1000 * 0.97;
                const slack = target - Date.now();
                if (slack > 0) await new Promise((r) => setTimeout(r, slack));
            }
        }
        return Date.now() - t0;
    }
}
