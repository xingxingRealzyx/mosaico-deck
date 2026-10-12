// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// index.js — mosaico-deck as a native DeepSeek Harness plugin.
//
// Two things this does that an MCP tool cannot:
//
// 1. The screen follows the agent automatically, with zero cooperation from the
//    model. It listens to agent/status, tools/pre-execute, tools/post-execute
//    and session/event, so the display moves on its own.
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
// Actual signatures, from dsh-tools/lib/types, dsh-agent/lib/types and
// dsh-session/lib/types:
//   tools/pre-execute   (exec, next) => Promise<PreToolDecision>           waterfall
//   tools/post-execute  (exec, result, next) => Promise<PostToolDecision>  waterfall
//   agent/status        (payload: { agent, status }) => void                emit
//   agent/error         (payload: { agent, turn, step, error }) => void     emit
//   session/event       (session, event: SessionEvent) => void              emit
//
// `assistant/message` is an event **type** carried by `session/event`, not an event
// name -- see the handler below, and the name guard in selftest.mjs that keeps this
// class of mistake from passing the load self-test again.
//
// Handlers below forward `next` when it exists and return undefined when it
// does not, which is correct for both modes.

import { defineTool } from '@deepseek-ai/dsh-tools';

import crypto from 'node:crypto';
import fsSync from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { Deck, findPort } from './device.js';
import { render, checkEnvironment } from './render.js';
import { textToPcm } from './tts.js';

export const name = 'mosaico-deck';
// `agents` tells the root conversation from the subagent sessions on the same bus;
// `sessionProjections` is the host's own fold of the session numbers the strip shows.
export const inject = ['tools', 'agents', 'sessionProjections'];

/** Short hash of this very file, reported at load and by deck_connect.
 *
 * Why it exists: **editing the plugin's JavaScript does not take effect until the
 * Harness process restarts.** Node caches ES modules by URL for the life of the
 * process, so unloading and re-applying the bundle re-runs apply() on the *cached*
 * old module -- the port reconnects, the tools re-register, and nothing looks wrong
 * while the new code silently never runs. That cost a confusing round of "the panel
 * still shows no image" debugging.
 *
 * With this stamp, "which build is live?" is one deck_connect away instead of a guess.
 */
export const build = (() => {
    try {
        const self = fileURLToPath(import.meta.url);
        return crypto.createHash('sha256').update(fsSync.readFileSync(self))
            .digest('hex').slice(0, 7);
    } catch {
        return 'unknown';
    }
})();

/**
 * Should a finished turn be announced out loud?
 *
 * Pure and exported so the rule can be tested without a speech synthesiser, and so the
 * **policy** is readable in one place. Speaking is for the moments you are not looking
 * at the screen: a turn that ran for half a minute, a failure, a question waiting for
 * an answer. A three-second exchange is not one of them.
 */
export function shouldAnnounce({ voiceOn, elapsedMs, sinceLastMs, minMs, minGapMs }) {
    return Boolean(voiceOn) && elapsedMs >= minMs && sinceLastMs >= minGapMs;
}

// Port polling policy: **at most one attempt at startup, then nothing.**
//
// The firmware splits USB CDC into a flashing port and a data port
// (CONFIG_TINYUSB_CDC_COUNT=2), so the plugin and a flasher no longer contend for
// one node. Opening the *flashing* node is still the reset signal, though, and
// this plugin must never risk touching it. Two rules keep it off:
//
//   . findPort() (device.js) accepts only the higher node of a *pair*, so a board
//     that is absent or sitting in ROM download mode -- where the composite device
//     is gone and at most one node exists -- yields no port at all.
//   . we try once at startup and then stop. Even a retry against a half-enumerated
//     device is enough to yank the chip out of ROM mode at the wrong moment;
//     observed symptom was the board re-enumerating every few seconds, never
//     staying in ROM mode, and flashing becoming impossible.
//
// So: try at startup, then back off hard. If the board shows up later, the user
// calls deck_connect; what must never happen is this plugin fighting an ongoing flash.
const TICK_MS = 1000;

// How long a picture stays on the panel before it clears itself.
//
// It used to stay until the next tool call, which in a quiet conversation means
// forever -- the user asked for it to go away on its own. Ten seconds is long enough
// to look at a screenshot and short enough that it never becomes the resting state.
//
// The env override exists so plugin/test-plugin-events.mjs can watch the expiry happen
// in milliseconds instead of sleeping ten seconds.
const IMAGE_HOLD_MS = Number(process.env.DECK_IMAGE_HOLD_MS || 10000);

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
        tool: null,     // current tool name, so the panel can pick an icon + colour
        image: null,    // a picture being looked at; drawn as a preview on status
        caption: null,  // optional line under a picture the model asked to show
        imageUntil: 0,  // wall clock at which the picture clears itself
        imageLeft: 0,   // last countdown value painted, so the ticker can notice
        progress: null,
        elapsed: 0,
        startedAt: 0,
        log: [],
        // Last numbers read from the host's session projections (see readStats), plus
        // the root session they belong to.
        rootSession: null,
        voiceName: process.env.DECK_VOICE_NAME || undefined,   // macOS voice, e.g. Tingting
        stats: {
            context: 0,       // pressure the compaction threshold is measured against
            contextWindow: 0,
            input: 0,
            output: 0,
            cacheHit: 0,
            tokensPerSecond: 0,
            turn: 0,
            steps: 0,
        },
        asking: null, // { resolve, options } while waiting for a tap
        audio: Buffer.alloc(0), // captured PCM while the user holds the key
    };

    const deck = new Deck(null);
    let buttons = [];
    let painting = false;

    // Log entries carry **what kind of thing happened**, not just a string: the
    // renderer turns the kind (and the tool name) into a colour emoji, which is what
    // makes the tail readable at a glance instead of a wall of grey text.
    const pushLog = (entry) => {
        state.log.push(entry);
        state.log = state.log.slice(-6);
    };

    // ---- picture lifetime ------------------------------------------------------
    // A picture is shown as a preview and then **clears itself**. The earlier rule was
    // "until the next tool call", which in a quiet conversation means it never goes
    // away at all -- the user asked for it to expire on its own.
    let imageTimer = null;

    const secondsOfImageLeft = () => (state.imageUntil > Date.now()
        ? Math.ceil((state.imageUntil - Date.now()) / 1000)
        : 0);

    function showImage(p, caption = null) {
        if (imageTimer) {
            clearTimeout(imageTimer);
            imageTimer = null;
        }
        state.image = p;
        state.caption = caption;
        state.imageUntil = p ? Date.now() + IMAGE_HOLD_MS : 0;
        if (p) {
            imageTimer = setTimeout(() => {
                imageTimer = null;
                state.image = null;
                state.caption = null;
                state.imageUntil = 0;
                void paint();
            }, IMAGE_HOLD_MS);
        }
    }

    async function paint({ force = false } = {}) {
        if (!deck.connected || painting) return 0;
        painting = true;
        try {
            const { rgb } = await render({
                // A picture is drawn as a preview *inside* the status page, not as a
                // full-screen takeover: the state label stays readable above it.
                page: 'status',
                image: state.image,
                imageLeft: secondsOfImageLeft(),
                caption: state.caption,
                caption: state.caption,
                project: state.project,
                state: state.status,
                tool: state.tool,
                detail: state.detail,
                target: state.target,
                progress: state.progress,
                elapsed: state.elapsed,
                log: state.log,
                stats: readStats(),
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

    // ---- voice ----------------------------------------------------------------
    // The board has had a speaker from the beginning and it was never used: speaking
    // was a tool the model had to remember to call, and it never did. Same lesson as
    // the screen -- **anything that should happen by itself must be driven by host
    // events, not by the model's goodwill.**
    const ANNOUNCE_MIN_MS = Number(process.env.DECK_ANNOUNCE_MS || 30000);
    // Both are env-overridable so the tests can watch a real announcement happen in
    // milliseconds instead of sleeping thirty seconds.
    const SPEAK_GAP_MS = Number(process.env.DECK_SPEAK_GAP_MS || 4000);
    let voiceOn = String(process.env.DECK_VOICE || '').toLowerCase() !== 'off';
    let lastSpokeAt = 0;
    let speechChain = Promise.resolve();

    /** Queue one utterance.
     *
     * **Serialized on purpose**: `speakPcm` paces itself to the playback rate, so two
     * overlapping utterances would interleave PCM on the wire and come out as noise.
     */
    function say(text, { respectGap = false } = {}) {
        const line = String(text ?? '').replace(/\s+/g, ' ').trim();
        if (!line || !voiceOn || !deck.connected) return;
        if (respectGap && Date.now() - lastSpokeAt < SPEAK_GAP_MS) return;
        speechChain = speechChain.then(async () => {
            try {
                const pcm = await textToPcm(line, state.voiceName);
                lastSpokeAt = Date.now();
                await deck.speakPcm(pcm);
            } catch (err) {
                ctx.logger?.debug?.(`mosaico-deck: speech failed: ${err.message}`);
            }
        });
    }

    /** Is this the user's own conversation, rather than a subagent's?
     *
     * Every live agent shares this event bus, so a subagent reading files and running
     * tests emits `assistant/message` just like the main loop does. Counting those
     * would quietly inflate the numbers the user is reading as *their* session. When
     * the registry cannot answer, count it: a slightly high number beats a blank panel.
     */
    function isRootSession(session) {
        try {
            const roots = ctx.agents?.roots?.();
            if (!Array.isArray(roots) || roots.length === 0) return true;
            return roots.some((a) => a?.session === session);
        } catch {
            return true;
        }
    }

    /** The user's own conversation, or null before one is known. */
    function rootSession() {
        try {
            const roots = ctx.agents?.roots?.();
            const withSession = Array.isArray(roots)
                ? roots.find((a) => a?.session) : undefined;
            if (withSession) return withSession.session;
        } catch {
            /* registry not reachable: fall back to whatever we saw on the bus */
        }
        return state.rootSession;
    }

    /** Read the host's own session numbers.
     *
     * **Do not recompute these.** `dsh-token-meter` and `dsh-session-stats` already
     * fold them into session projections, driven incrementally as the log grows, so
     * reading is both cheaper and more correct than replaying `assistant/message`
     * usage here -- which is what this used to do, and why the strip stayed empty.
     *
     *   contextPressure  { pressureTokens, contextWindow, surfaceTokens }
     *   tokenUsage       { totals: { uncachedInputTokens, outputTokens,
     *                               cacheReadTokens, cacheWriteTokens } }
     *   sessionStats     { turns, steps, llmMs, toolMs, ttftMs, decodeMs }
     */
    function readStats() {
        const session = rootSession();
        const snapshot = session && ctx.sessionProjections?.snapshot;
        if (!snapshot) return state.stats;
        try {
            const { values } = ctx.sessionProjections.snapshot(session,
                ['contextPressure', 'tokenUsage', 'sessionStats']);
            const pressure = values?.contextPressure ?? {};
            const totals = values?.tokenUsage?.totals ?? {};
            const stats = values?.sessionStats ?? {};
            const decodeMs = numOr0(stats.decodeMs);
            const decodeTokens = numOr0(stats.decodeTokens);
            const uncached = numOr0(totals.uncachedInputTokens);
            const cacheRead = numOr0(totals.cacheReadTokens);
            const read = uncached + cacheRead;
            return {
                // Matches the Harness UI exactly (`contextOccupancy`): it prefers
                // projectedTokens and falls back to pressureTokens, and the renderer
                // clamps the ratio to 100 the same way. The panel and the window must
                // never disagree about how full the context is.
                context: numOr0(pressure.projectedTokens ?? pressure.pressureTokens),
                contextWindow: numOr0(pressure.contextWindow),
                input: uncached,
                output: numOr0(totals.outputTokens),
                // Share of the prompt that came from cache. Computed here, against
                // uncached+cached: dividing by the uncached half alone yields absurd
                // percentages once caching works (this session reads 99.6%).
                cacheHit: read > 0 ? Math.round(100 * cacheRead / read) : 0,
                tokensPerSecond: decodeMs > 0 ? (decodeTokens / decodeMs) * 1000 : 0,
                turn: numOr0(stats.turns),
                steps: numOr0(stats.steps),
            };
        } catch {
            return state.stats;   // a projection that is missing is not an error
        }
    }

    function numOr0(v) {
        return Number.isFinite(v) ? v : 0;
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

    const IMAGE_EXT = /\.(png|jpe?g|webp|gif|bmp|tiff?|heic)$/i;
    const READ_TOOL = /^(read|read_image|open_image|view_image)$/i;

    /** The picture this call is about to look at, or null.
     *
     * Only **reads** count: a write that happens to end in .png should not throw a
     * picture up on the panel. The payoff is the point of having a panel at all --
     * when the agent studies a screenshot, the screenshot is on the desk instead of
     * being a path buried in a transcript.
     *
     * The image stays until the next tool call or assistant message replaces it, so
     * it is visible for as long as that step lasts rather than flashing by.
     */
    function imageBeingRead(toolName, target) {
        if (!target || !IMAGE_EXT.test(target) || !READ_TOOL.test(toolName)) return null;
        try {
            return path.isAbsolute(target) ? target : path.resolve(target);
        } catch {
            return target;
        }
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
            state.detail = '';   // not 'Working' -- the label already says 思考中
            state.target = '';
            state.tool = null;
        } else {
            // The turn just ended. Announce it only when it was long enough to have
            // been worth walking away from -- see shouldAnnounce().
            const elapsed = state.startedAt ? Date.now() - state.startedAt : 0;
            if (shouldAnnounce({
                voiceOn,
                elapsedMs: elapsed,
                sinceLastMs: Date.now() - lastSpokeAt,
                minMs: ANNOUNCE_MIN_MS,
                minGapMs: SPEAK_GAP_MS,
            })) {
                const mins = Math.floor(elapsed / 60000);
                say(mins >= 1 ? `完成，用时 ${mins} 分` : '完成');
            }
            state.status = 'idle';
            state.tool = null;
        }
        void paint();
    });

    // waterfall: must forward next()
    ctx.on('tools/pre-execute', (exec, next) => {
        try {
            const { toolName, target } = describeTool(exec);
            state.status = 'tool';
            state.tool = toolName;
            state.detail = `Calling ${toolName}`;
            if (target) state.target = target.slice(0, 40);
            state.progress = null;
            // A read of a picture shows the picture (and it clears itself); any other
            // call takes it down.
            showImage(imageBeingRead(toolName, target));
            pushLog({ kind: 'tool', tool: toolName, text: `called ${toolName}` });
            if (state.image) {
                pushLog({ kind: 'image', tool: toolName, text: 'showing image' });
            }
        } catch {
            /* payload shape changed; never break the tool pipeline over this */
        }
        void paint();
        return typeof next === 'function' ? next() : undefined;
    });

    // waterfall with three parameters: (exec, result, next)
    ctx.on('tools/post-execute', (exec, result, next) => {
        try {
            // **No "Tool finished".** It is the same string after every call, so it
            // tells the user nothing -- the state label already reads 思考中 and the
            // log line already names the tool that ran. Rule for this panel: the
            // detail line must not restate the state label.
            state.status = 'thinking';
            state.tool = null;
            state.detail = '';
        } catch {
            /* ignore */
        }
        void paint();
        return typeof next === 'function' ? next() : undefined;
    });

    // `assistant/message` is a **session event type, not a bus event**: the bus carries
    // it inside `session/event`, whose second argument is the logged event.
    //
    //     session/event(session, event)                                        emit
    //     event = { type: 'assistant/message', turn, step, message, stream, usage? }
    //
    // Subscribing to the bare name was dead code: it never fired, so the assistant
    // text and the "replied" log line never appeared on the device. It went unnoticed
    // because the load self-test fires handlers by name; selftest.mjs now also asserts
    // that every subscribed name is part of the host's real event surface.
    //
    // The session numbers are **not** accumulated here. They used to be, from this
    // event's `usage` field -- and the panel stayed empty, because that field does not
    // reach a listener in practice. The host already computes all of it as session
    // projections (see readStats); reading its numbers is both correct and cheaper.
    ctx.on('session/event', (session, event) => {
        if (event?.type !== 'assistant/message') return;
        if (isRootSession(session)) state.rootSession = session;
        try {
            const content = event.message?.content;
            const text = Array.isArray(content)
                ? content.map((c) => c?.text ?? '').join('')
                : '';
            if (text) {
                state.status = 'running';
                state.tool = null;
                // **The picture stays.** A reply is usually *about* the picture the
                // agent just looked at, so clearing it here would blank the panel at
                // exactly the wrong moment -- the user reading the explanation wants
                // the image still on the desk. Only the next non-image tool call
                // replaces it.
                state.detail = text.replace(/\s+/g, ' ').slice(0, 34);
                pushLog({ kind: 'reply', text: 'replied' });
            }
        } catch {
            /* ignore */
        }
        void paint();
    });

    ctx.on('agent/error', (payload) => {
        state.status = 'error';
        state.tool = null;
        // Not the constant 'Error' (the label already reads 出错): put the **actual
        // failure** on the panel. That is the one thing here the user cannot get
        // without switching windows, which is the whole point of the device.
        state.detail = errorText(payload?.error);
        pushLog({ kind: 'error', text: 'error' });
        // A failure is worth interrupting a room for, and it carries the one piece of
        // information you cannot get without switching windows.
        say(state.detail ? `出错。${state.detail}` : '出错', { respectGap: true });
        void paint({ force: true });
    });

    /** One readable line for a failure, whatever shape the payload arrived in.
     *
     * `agent/error` carries `error: unknown` -- an Error, a plain object, a string, or
     * anything the provider threw. Never let summarising it throw: a broken formatter
     * inside an error handler would be a nasty little trap.
     */
    function errorText(err) {
        if (err === undefined || err === null) return '';
        let text = '';
        try {
            if (err instanceof Error) text = err.message;
            else if (typeof err === 'object') text = err.message ?? JSON.stringify(err);
            else text = String(err);
        } catch {
            text = 'unprintable error';
        }
        return text.replace(/\s+/g, ' ').slice(0, 40);
    }

    function connect() {
        if (deck.connected) return true;
        if (!deck.open()) return false;
        state.connected = true;
        setTimeout(() => paint({ force: true }), FIRST_PAINT_MS);
        return true;
    }

    if (!connect()) {
        ctx.logger?.info?.(
            'mosaico-deck: board not found -- a pair of /dev/cu.usbmodem* nodes is '
            + 'required, so a board that is unplugged or in ROM download mode is '
            + 'ignored. Not probing again (opening the flashing node asserts DTR/RTS). '
            + 'Call deck_connect once the board is running.',
        );
    }

    // Cleanup must go through `ctx.effect()`.
    //
    // **Cordis never emits a `dispose` event** -- there is no such name in any of the
    // host's `Events` interfaces -- so `ctx.on('dispose', ...)` silently never ran and
    // both the interval and the open serial fd outlived the plugin. That is how a
    // stale process kept holding /dev/cu.usbmodem21201 across a restart. Listener
    // registrations unwind with the fiber on their own; raw resources do not.
    ctx.effect(() => {
        const tickTimer = setInterval(() => {
            if (!deck.connected) return;
            const next = state.status === 'idle'
                ? 0
                : Math.floor((Date.now() - state.startedAt) / 1000);
            // The picture countdown has to tick while the agent is idle too, so it
            // cannot ride on the elapsed-seconds check alone.
            const left = secondsOfImageLeft();
            if (next !== state.elapsed || left !== state.imageLeft) {
                state.elapsed = next;
                state.imageLeft = left;
                void paint();
            }
        }, TICK_MS);

        return () => {
            clearInterval(tickTimer);
            if (imageTimer) clearTimeout(imageTimer);
            deck.close();
        };
    });

    // -----------------------------------------------------------------------
    // Tools
    // -----------------------------------------------------------------------
    const textOutput = (pick) => ({
        schema: { type: 'json' },
        render: (_args, value) => [{ type: 'text', text: pick(value) }],
    });

    ctx.tools.register(defineTool({
        name: 'deck_connect',
        description:
            'Connect to the mosaico-deck device. The plugin tries once at startup; if the '
            + 'device was not attached then (not plugged in, or the chip was in ROM '
            + 'download mode) call this after it is running. Opening the serial port '
            + 'asserts DTR/RTS, which is the reset signal in download mode, so this is '
            + 'deliberately explicit rather than automatic.',
        parameters: {},
        output: textOutput((v) => v.message),
        async execute() {
            if (deck.connected) return { message: `already connected to ${deck.port} (build ${build})` };
            if (!connect()) {
                return {
                    message: 'no mosaico-deck data port found: the board must be running '
                        + '(not in ROM download mode) and enumerate as a pair of '
                        + '/dev/cu.usbmodem* nodes; the higher node of the pair is used',
                };
            }
            return { message: `connected to ${deck.port} (build ${build})` };
        },
        presentCall: () => ({ card: 'generic', kind: 'read', title: 'Connect to device' }),
    }));

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

            // Read the question out: the whole point of asking on the device is that
            // the user is not looking at a window.
            say(String(args.question));
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
            if (args.logLine) pushLog({ kind: 'info', text: String(args.logLine) });
            const n = await paint();
            return { message: n ? `updated (${n} bands)` : 'no change' };
        },
        presentCall: () => ({ card: 'generic', kind: 'read', title: 'Update device display' }),
    }));

    ctx.tools.register(defineTool({
        name: 'deck_image',
        description:
            'Show a picture on the mosaico-deck device: a screenshot, a diagram, a photo. '
            + 'Reading an image with the normal read tool already does this automatically, '
            + 'so reach for this only when you want a specific picture on the panel -- '
            + 'for example to show the user what you are looking at, or to compare two '
            + 'images side by side over successive calls.',
        parameters: {
            path: { type: 'string', required: true, description: 'Image file to display.' },
            caption: { type: 'string', description: 'One line to show under the picture.' },
        },
        output: textOutput((v) => v.message),
        async execute(args) {
            if (!deck.connected) return { message: 'device not connected' };
            const p = String(args.path ?? '');
            if (!p) return { message: 'no path given' };
            state.status = 'tool';
            state.tool = 'deck_image';
            showImage(path.isAbsolute(p) ? p : path.resolve(p),
                      args.caption ? String(args.caption) : null);
            const n = await paint({ force: true });
            return {
                message: n
                    ? `showing ${state.image} for ${IMAGE_HOLD_MS / 1000}s (${n} bands)`
                    : `asked for ${state.image}, but the panel did not change`,
            };
        },
        presentCall: (a) => ({
            card: 'generic',
            kind: 'read',
            title: `Show: ${String(a.path ?? '').split('/').pop().slice(0, 20)}`,
        }),
    }));

    ctx.tools.register(defineTool({
        name: 'deck_voice',
        description:
            'Turn the mosaico-deck speaker on or off. Announcements are automatic -- a '
            + 'failure, a question waiting for an answer, or a turn that ran long -- so '
            + 'this is the mute switch, e.g. before a meeting.',
        parameters: {
            enabled: { type: 'boolean', description: 'Omit to just report the current state.' },
        },
        output: textOutput((v) => v.message),
        async execute(args) {
            if (args.enabled !== undefined) voiceOn = Boolean(args.enabled);
            return {
                message: `voice ${voiceOn ? 'on' : 'off'}`
                    + (deck.connected ? '' : ' (device not connected)'),
            };
        },
        presentCall: () => ({ card: 'generic', kind: 'read', title: 'Deck voice' }),
    }));

    ctx.logger?.info?.(
        `mosaico-deck: build=${build} python=${checkEnvironment().python} `
        + `port=${findPort() ?? 'none'}`,
    );
}
