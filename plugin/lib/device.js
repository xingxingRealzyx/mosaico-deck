// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// device.js —— 和 mosaico-deck 板子通信（纯 Node，无原生模块）
//
// 为什么要"纯 Node"
// ----------------
// 这个插件跑在 DeepSeek Harness 里，而 **DSH 是签名过的 app**，任何原生 Node
// 模块（serialport、sharp 之类）都会加载失败：
//
//     code signature not valid for use in process:
//     mapping process and mapped file (non-platform) have different Team IDs
//
// 这不是配置问题，是 macOS 的库验证。所以：
//
//   · **串口**：`stty` 把 tty 设成 raw，然后用 `fs` 直接读写设备文件。
//     不需要 serialport。USB CDC 的波特率本来就是虚拟的 —— 设备根本不看，
//     真正要紧的是**行规程必须关掉**（默认的 canonical 模式会改写二进制数据：
//     换行被转换、0x11/0x13 被当流控吃掉）。
//
//   · **渲染**：交给 DSH 自带的那份 Python（3.12 + Pillow 都在 runtime 里），
//     见 render.js。

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

const AUDIO_BYTES_PER_SEC = 48000 * 4; // 48kHz 交错立体声 16 位

/** 找一个 mosaico-deck 的串口。
 *
 * 按名字优先 —— 系统里可能插着别的 usbmodem 设备。
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
        this._last = null; // 上一帧的 RGB，用来做 band 差分
        this._rx = Buffer.alloc(0);
        this._pendingAudio = 0;
        this._timer = null;
        this.stats = { bands: 0, bytes: 0, lastMs: 0, fulls: 0 };
    }

    get connected() {
        return this.fd !== null;
    }

    /** 打开串口。失败返回 false（不抛）—— 板子没插是很正常的状态。 */
    open() {
        if (this.fd !== null) return true;
        this.port = this.port || findPort();
        if (!this.port) return false;
        try {
            // 必须先设 raw 再打开：否则中间那段时间收到的数据已经被行规程
            // 处理过了。
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
                /* 已经没了 */
            }
            this.fd = null;
        }
        this.emit('close');
    }

    // ---- 收 ---------------------------------------------------------------
    _pump() {
        if (this.fd === null) return;
        const buf = Buffer.alloc(16384);
        for (;;) {
            let n = 0;
            try {
                n = fs.readSync(this.fd, buf, 0, buf.length, null);
            } catch (e) {
                if (e.code === 'EAGAIN') break;
                // 板子被拔了 / 复位了。关掉并让上层去重连。
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
            // 录音是"一行 JSON 报长度 + 裸 PCM"，所以要先看有没有待收的裸数据。
            // **不能当行读** —— PCM 里全是 \n。
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
            if (!line.startsWith('{')) continue; // 板子的日志，忽略
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

    // ---- 发 ---------------------------------------------------------------
    /** 往 tty 写，**必须处理 EAGAIN**。
     *
     * 文件是 O_NONBLOCK 打开的，而整屏 450KB 一次写下去会超过 tty 的缓冲，
     * 于是 writeSync 抛 EAGAIN。这不是错误，只是"现在写不进去，等会儿" ——
     * 退让一下重试即可，否则一推整屏就崩。
     */
    _writeAll(buf) {
        let off = 0;
        while (off < buf.length) {
            try {
                off += fs.writeSync(this.fd, buf, off, buf.length - off);
            } catch (e) {
                if (e.code === 'EAGAIN') {
                    // 睡 1ms 再试。这里不能 await（调用方是同步的渲染循环），
                    // 用 Atomics.wait 做一次极短的同步等待。
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

    /** 把 RGB（480×480×3）推上去，**只发变化的那几条 band**。
     *
     * 这是整套设计的关键：整屏 450KB 在 60MHz QSPI 上要 15.4ms，而面板刷新
     * 周期是 17ms —— 几乎相等，稍微一抖就跨越刷新、画面撕裂。而只发改动的
     * 那几条（一行字约 1 条 band = 8.7ms）稳稳落在一次刷新之内。
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
                // 标准 RGB565，高字节是 R5G3、低字节是 G3B5。
                const v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (bl >> 3);
                // ⚠️ **必须高字节在前。**
                //
                // 固件那边是 `buf[i] = swap16((r5 << 11) | b5)`，而 buf 是
                // uint16_t、CPU 是小端 —— 所以落到内存里的顺序是
                // [RGB565 高字节][低字节]。
                //
                // 这里一开始写反了（低字节在前），症状很有迷惑性：
                //   · 黑 0x0000 交换后还是黑 -> 背景看着正常
                //   · 白 0xFFFF 交换后还是白 -> 主文字看着正常
                //   · **彩色全串** -> 强调色文字、进度条、状态点颜色全错
                // 于是看起来就像"字体颜色不对"。
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

    /** 推一段 48kHz 交错立体声 16 位 PCM 去播放。
     *
     * **必须按播放速率发。** 板子是边收边播的，播放速度被 I2S DMA 钉死在
     * 192KB/s；一口气灌进去的话它的接收缓冲瞬间就满了（实测 99 个分块只收到
     * 5 个）。0.97 倍速是实测出来的：4 秒的音频只超前约 16KB，稳在 64KB 缓冲内。
     */
    async speakPcm(pcm, { chunk = 2048 } = {}) {
        if (this.fd === null) throw new Error('板子没连上');
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
