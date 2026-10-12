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
        return fs.readFileSync(pcm);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}
