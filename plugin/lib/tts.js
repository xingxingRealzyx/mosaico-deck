// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// tts.js -- text to the PCM format the board plays.
//
// **TTS lives on the PC on purpose**: the board has 512 KB of SRAM and no neural
// acceleration, so it cannot run any real TTS. macOS `say` is high quality and speaks
// Chinese out of the box; ffmpeg does the container conversion. The board only plays
// samples.
//
// Output must be 48 kHz interleaved stereo 16-bit, matching the I2S/codec
// configuration in components/audio.
//
// It sits in its own module so the *decision* to speak -- the interesting part, and the
// part that used to be missing entirely -- can be tested without a speech synthesiser:
// plugin/test-plugin-events.mjs swaps this file for a stub that records what would
// have been said.

import { execFile } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { promisify } from 'node:util';

const run = promisify(execFile);

/**
 * Scale the samples up so the loudest one sits just under full scale.
 *
 * **This is where the missing volume was.** macOS `say` hands back a signal about 7 dB
 * below full scale (measured: peak 14616/32767 = 44.6%), so the codec volume was being
 * asked to make up for a quiet source and could not -- turning it up only got closer to
 * a ceiling the source had already set. Speech stayed quiet at every volume setting.
 *
 * The headroom is real, not a boost: the peak is measured first and the gain is chosen to
 * land just under 0 dBFS, so nothing clips except material that was already at maximum.
 *
 * @param {Buffer} pcm 48 kHz interleaved stereo s16le, modified in place semantically
 * @returns {Buffer} the same audio, normalized (or the input if it is already loud)
 */
export function normalize(pcm) {
    const samples = pcm.length >> 1;
    let peak = 0;
    for (let i = 0; i < samples; i++) {
        const v = Math.abs(pcm.readInt16LE(i * 2));
        if (v > peak) peak = v;
    }
    if (peak === 0) return pcm;
    // Cap the gain at +18 dB: silence-with-a-click should not become a bang.
    const gain = Math.min((32767 * 0.95) / peak, 8);
    if (gain <= 1.05) return pcm;
    const out = Buffer.alloc(pcm.length);
    for (let i = 0; i < samples; i++) {
        let v = Math.round(pcm.readInt16LE(i * 2) * gain);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        out.writeInt16LE(v, i * 2);
    }
    return out;
}

/**
 * Synthesize one sentence into raw PCM.
 *
 * @param {string} text what to say
 * @param {string} [voice] macOS voice name, e.g. `Tingting`
 * @returns {Promise<Buffer>} 48 kHz interleaved stereo s16le
 */
export async function textToPcm(text, voice) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-'));
    const aiff = path.join(dir, 't.aiff');
    const pcm = path.join(dir, 't.pcm');
    try {
        const sayArgs = ['-o', aiff];
        if (voice) sayArgs.push('-v', voice);
        sayArgs.push(text);
        await run('say', sayArgs, { timeout: 60000 });
        await run('ffmpeg', [
            '-y', '-loglevel', 'error', '-i', aiff,
            '-f', 's16le', '-acodec', 'pcm_s16le', '-ar', '48000', '-ac', '2', pcm,
        ], { timeout: 60000 });
        return normalize(fs.readFileSync(pcm));
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}
