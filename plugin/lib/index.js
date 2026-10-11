// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// index.js — mosaico-deck as a native DeepSeek Harness plugin.
//
// Two things this does that an MCP tool cannot:
//
// 1. The screen follows the agent automatically, with zero cooperation from the
//    model. It listens to agent/status, tools/pre-execute, tools/post-execute
//    and assistant/message, so the display moves on its own.
//
// 2. What is shown is host-observed fact, not the model's self-report. The tool
//    name comes from the runtime; "I am running the tests" comes from the model
//    and may be fiction.
//
// ---------------------------------------------------------------------------
// Event modes: check before subscribing
// ---------------------------------------------------------------------------
// Cordis events have several modes and the handler signature depends on which.
// Getting it wrong does not fail at load time — it breaks the pipeline at call
// time. That happened here: tools/pre-execute is a `waterfall`, the handler was
// written as a plain `emit` listener and never called next(), so every tool
// call in the host threw:
//
//     Cannot read properties of undefined (reading 'kind')
//
// Actual signatures, from dsh-tools/lib/types:
//   tools/pre-execute   (exec, next) => Promise<PreToolDecision>           waterfall
//   tools/post-execute  (exec, result, next) => Promise<PostToolDecision>  waterfall
//   agent/status        (payload: { agent, status }) => void                emit
//   agent/error         (payload) => void                                   emit
//
// Handlers below forward `next` when it exists and return undefined when it
// does not, which is correct for both modes.

import { defineTool } from '@deepseek-ai/dsh-tools';

import { Deck, findPort } from './device.js';
import { render, checkEnvironment } from './render.js';

export const name = 'mosaico-deck';
export const inject = ['tools'];

// Reconnect backoff. **Do not poll the port aggressively.**
//
// On this board, opening /dev/cu.usbmodem* asserts DTR and RTS -- that is a
// hardware fact, not a firmware quirk (flash-rom.sh documents the same trap).
// While the chip sits in ROM download mode those lines ARE the reset signal, so
// a plugin that periodically opens the port to "see if the board is there"
// kicks the chip straight back out of download mode. Observed symptom: the
// board re-enumerates every few seconds, never stays in ROM mode, and flashing
// becomes impossible.
//
// So: try at startup, then back off hard and stop retrying. If the board shows
// up later, the user can reconnect from the host; what must never happen is
// this plugin fighting an ongoing flash.
const RECONNECT_MS = 5000;
const RECONNECT_MAX = 3; // then give up and leave the port alone
const TICK_MS = 1000;

// Wait this long after connecting before pushing the first frame. The board is
// running its boot animation (~1.9s) and painting over it would fight for the
// SPI bus. The animation ends at boot::kScatterEndUs in main.cpp and then idles
// in a settle state, so this is a coordination constant, not a guess.
const FIRST_PAINT_MS = 1600;

export function apply(ctx) {
    const state = {
        connected: false,
        project: 'Harness',
        status: 'idle',
        detail: '',
        target: '',
        progress: null,
        elapsed: 0,
        startedAt: 0,
        log: [],
        asking: null, // { resolve, options } while waiting for a tap
        audio: Buffer.alloc(0), // captured PCM while the user holds the key
    };

    const deck = new Deck(null);
    let buttons = [];
    let painting = false;

    const pushLog = (line) => {
        state.log.push(line);
        state.log = state.log.slice(-6);
    };

    async function paint({ force = false } = {}) {
        if (!deck.connected || painting) return 0;
        painting = true;
        try {
            const { rgb } = await render({
                page: 'status',
                project: state.project,
                state: state.status,
                detail: state.detail,
                target: state.target,
                progress: state.progress,
                elapsed: state.elapsed,
                log: state.log,
                files: state.files,
                testsPassed: state.testsPassed,
                testsTotal: state.testsTotal,
                // Tap targets exist only while a question is pending. Drawing
                // them otherwise is noise, and a stray tap is expensive.
                buttons: state.asking ? null : undefined,
            });
            return deck.sendRgb(rgb, { force });
        } catch (err) {
            // A broken screen must never take the agent down with it.
            ctx.logger?.debug?.(`mosaico-deck: paint failed: ${err.message}`);
            return 0;
        } finally {
            painting = false;
        }
    }

    /** Pull a readable tool name and argument out of a ToolExecution.
     *
     * The exact payload shape is not a contract worth relying on, so probe a
     * few plausible fields and degrade to a generic label.
     */
    function describeTool(exec) {
        const call = exec?.call ?? exec?.toolCall ?? exec?.tool ?? exec;
        const rawName = call?.name ?? call?.toolName ?? call?.tool ?? 'tool';
        const args = call?.arguments ?? call?.args ?? exec?.arguments;
        let target = '';
        if (args && typeof args === 'object') {
            const first = args.command ?? args.path ?? args.file_path ?? args.query;
            if (first) target = String(first);
        }
        return { toolName: String(rawName), target };
    }

    deck.on('event', (ev) => {
        if (ev.t === 'touch' && ev.ev === 'down' && state.asking) {
            const hit = buttons.find(
                (b) => ev.x >= b.x - 6 && ev.x <= b.x + b.w + 6
                    && ev.y >= b.y - 6 && ev.y <= b.y + b.h + 6,
            );
            if (!hit) return;
            const pending = state.asking;
            state.asking = null;
            buttons = [];
            const value = String(hit.value);
            pending.resolve(Number.isInteger(+value) ? pending.options[+value] : value);
        }
    });

    deck.on('key', () => {
        // The physical key cancels a pending question. Easier to find by touch
        // than an on-screen cancel.
        if (!state.asking) return;
        const pending = state.asking;
        state.asking = null;
        buttons = [];
        pending.resolve('cancelled');
    });

    deck.on('audio', (pcm) => {
        state.audio = Buffer.concat([state.audio, pcm]);
    });

    // -----------------------------------------------------------------------
    // Lifecycle: this is what makes the screen follow the agent
    // -----------------------------------------------------------------------
    ctx.on('agent/status', (payload) => {
        if (payload?.status === 'running') {
            if (state.status === 'idle') {
                state.startedAt = Date.now();
                state.log = [];
            }
            state.status = 'thinking';
            state.detail = 'Working';
            state.target = '';
        } else {
            state.status = 'idle';
        }
        void paint();
    });

    // waterfall: must forward next()
    ctx.on('tools/pre-execute', (exec, next) => {
        try {
            const { toolName, target } = describeTool(exec);
            state.status = 'tool';
            state.detail = `Calling ${toolName}`;
            if (target) state.target = target.slice(0, 34);
            state.progress = null;
            pushLog(`called ${toolName}`);
        } catch {
            /* payload shape changed; never break the tool pipeline over this */
        }
        void paint();
        return typeof next === 'function' ? next() : undefined;
    });

    // waterfall with three parameters: (exec, result, next)
    ctx.on('tools/post-execute', (exec, result, next) => {
        try {
            state.status = 'thinking';
            state.detail = 'Tool finished';
        } catch {
            /* ignore */
        }
        void paint();
        return typeof next === 'function' ? next() : undefined;
    });

    ctx.on('assistant/message', (payload, next) => {
        try {
            const content = payload?.message?.content;
            const text = Array.isArray(content)
                ? content.map((c) => c?.text ?? '').join('')
                : '';
            if (text) {
                state.status = 'running';
                state.detail = text.replace(/\s+/g, ' ').slice(0, 30);
                pushLog('replied');
            }
        } catch {
            /* ignore */
        }
        void paint();
        return typeof next === 'function' ? next() : undefined;
    });

    ctx.on('agent/error', () => {
        state.status = 'error';
        state.detail = 'Error';
        pushLog('error');
        void paint({ force: true });
    });

    function connect() {
        if (deck.connected) return true;
        if (!deck.open()) return false;
        state.connected = true;
        setTimeout(() => paint({ force: true }), FIRST_PAINT_MS);
        return true;
    }

    connect();
    let attempts = 1;
    const reconnectTimer = setInterval(() => {
        if (deck.connected || attempts >= RECONNECT_MAX) {
            if (attempts >= RECONNECT_MAX && !deck.connected) {
                clearInterval(reconnectTimer);
                ctx.logger?.info?.('mosaico-deck: board not found, stopped probing the port');
            }
            return;
        }
        attempts += 1;
        connect();
    }, RECONNECT_MS);

    const tickTimer = setInterval(() => {
        if (!deck.connected) return;
        const next = state.status === 'idle'
            ? 0
            : Math.floor((Date.now() - state.startedAt) / 1000);
        if (next !== state.elapsed) {
            state.elapsed = next;
            void paint();
        }
    }, TICK_MS);

    ctx.on('dispose', () => {
        clearInterval(reconnectTimer);
        clearInterval(tickTimer);
        deck.close();
    });

    // -----------------------------------------------------------------------
    // Tools
    // -----------------------------------------------------------------------
    const textOutput = (pick) => ({
        schema: { type: 'json' },
        render: (_args, value) => [{ type: 'text', text: pick(value) }],
    });

    ctx.tools.register(defineTool({
        name: 'deck_speak',
        description:
            'Speak a sentence through the mosaico-deck device on the desk. Use it for '
            + 'done or take-a-look moments that should not interrupt the user but should '
            + 'be noticed.',
        parameters: {
            text: { type: 'string', required: true, description: 'What to say.' },
            voice: { type: 'string', description: 'macOS voice name, for example Tingting.' },
        },
        output: textOutput((v) => v.message),
        async execute(args) {
            if (!deck.connected) return { ok: false, message: 'device not connected' };
            const pcm = await textToPcm(args.text, args.voice);
            const ms = await deck.speakPcm(pcm);
            return { ok: true, message: `spoke ${(pcm.length / 192000).toFixed(1)}s of audio in ${ms}ms` };
        },
        presentCall: (a) => ({
            card: 'generic',
            kind: 'read',
            title: `Speak: ${(a.text ?? '').slice(0, 20)}`,
        }),
    }));

    ctx.tools.register(defineTool({
        name: 'deck_ask',
        description:
            'Ask the user a question on the mosaico-deck device and block until they '
            + 'answer. This is the most useful thing the device does: the user does not '
            + 'have to switch windows or find a prompt in a terminal, the device lights up '
            + 'and one tap answers it. Returns the chosen option text, or "timeout" if '
            + 'nobody answered, or "cancelled" if the user pressed the physical key.',
        parameters: {
            question: { type: 'string', required: true },
            options: {
                type: 'array',
                items: { type: 'string' },
                required: true,
                description: 'Two to five options; more than five cannot be tapped reliably.',
            },
            timeoutSec: { type: 'number', description: 'Default 120.' },
        },
        output: textOutput((v) => v.answer),
        async execute(args, exec) {
            if (!deck.connected) return { answer: 'device not connected' };
            const options = (args.options ?? []).slice(0, 5);
            const timeoutMs = (args.timeoutSec ?? 120) * 1000;

            const answer = await new Promise((resolve) => {
                state.asking = { resolve, options };
                void (async () => {
                    const { rgb, hits } = await render({
                        page: 'ask',
                        question: args.question,
                        options,
                        footer: 'Tap an option, or press the AI key to cancel',
                    });
                    buttons = hits;
                    await deck.sendRgb(rgb, { force: true });
                })();

                const timer = setTimeout(() => {
                    if (!state.asking) return;
                    state.asking = null;
                    buttons = [];
                    resolve('timeout');
                }, timeoutMs);

                // If the agent turn is aborted, stop waiting so the device does
                // not stay stuck on the question page.
                exec?.signal?.addEventListener?.('abort', () => {
                    clearTimeout(timer);
                    if (!state.asking) return;
                    state.asking = null;
                    buttons = [];
                    resolve('cancelled');
                }, { once: true });
            });

            void paint({ force: true });
            return { answer };
        },
        presentCall: (a) => ({
            card: 'generic',
            kind: 'read',
            title: `Ask: ${(a.question ?? '').slice(0, 20)}`,
        }),
    }));

    ctx.tools.register(defineTool({
        name: 'deck_show',
        description:
            'Set what the mosaico-deck displays. Rarely needed: the display already '
            + 'follows the agent automatically through host events.',
        parameters: {
            detail: { type: 'string' },
            target: { type: 'string' },
            state: {
                type: 'string',
                enum: ['idle', 'thinking', 'running', 'editing', 'testing', 'waiting', 'done', 'error'],
            },
            progress: { type: 'number', description: 'Zero to one.' },
            logLine: { type: 'string' },
        },
        output: textOutput((v) => v.message),
        async execute(args) {
            if (args.detail !== undefined) state.detail = String(args.detail);
            if (args.target !== undefined) state.target = String(args.target);
            if (args.state !== undefined) state.status = String(args.state);
            if (args.progress !== undefined) state.progress = Number(args.progress);
            if (args.logLine) pushLog(String(args.logLine));
            const n = await paint();
            return { message: n ? `updated (${n} bands)` : 'no change' };
        },
        presentCall: () => ({ card: 'generic', kind: 'read', title: 'Update device display' }),
    }));

    ctx.logger?.info?.(
        `mosaico-deck: python=${checkEnvironment().python} port=${findPort() ?? 'none'}`,
    );
}

/** Synthesize speech into the PCM format the board expects.
 *
 * TTS lives on the PC on purpose: the board has 512 KB of SRAM and no neural
 * acceleration, so it cannot run any real TTS. macOS say is high quality and
 * speaks Chinese out of the box. The board only plays the samples.
 *
 * Output must be 48 kHz interleaved stereo 16-bit, matching the I2S/codec
 * configuration in components/audio.
 */
async function textToPcm(text, voice) {
    const { execFile } = await import('node:child_process');
    const os = await import('node:os');
    const path = await import('node:path');
    const fs = await import('node:fs');
    const { promisify } = await import('node:util');
    const run = promisify(execFile);

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
