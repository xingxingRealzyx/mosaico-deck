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

import { execFileSync } from 'node:child_process';
import crypto from 'node:crypto';
import fsSync from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { Deck, findPort } from './device.js';
import { render, checkEnvironment } from './render.js';
import { textToPcm } from './tts.js';

/**
 * Which external programs this machine actually has.
 *
 * **A missing PC-side dependency shows up as a symptom, not an error**: no ffmpeg and
 * the speaker is silent, no Pillow and the panel stays blank, no whisper model and
 * push-to-talk just does nothing. Someone installing this project from the repository
 * should not have to guess which of those they are looking at, so the answer is probed
 * once at load and reported by `deck_connect` and the startup log.
 * `./check-deps.sh` prints the same list with install instructions.
 */
export const deps = (() => {
    // Test seam, in the same spirit as DECK_IMAGE_HOLD_MS: the suites must not depend on
    // which optional programs the machine running them happens to have.
    if (process.env.DECK_ASSUME_DEPS === '1') {
        return { ffmpeg: true, say: true, whisper: true, whisperModel: true,
                 whisperName: 'whisper-cli' };
    }
    const has = (cmd) => {
        try {
            execFileSync('/usr/bin/env', ['which', cmd], { stdio: 'ignore' });
            return true;
        } catch {
            return false;
        }
    };
    const whisperName = process.env.DECK_WHISPER || 'whisper-cli';
    const whisperModel = process.env.DECK_WHISPER_MODEL
        || path.join(os.homedir(), '.cache', 'whisper', 'ggml-small.bin');
    return {
        // Speech out needs both: `say` synthesizes, ffmpeg converts to what the board plays.
        ffmpeg: has('ffmpeg'),
        // macOS-only, and the one genuinely platform-bound piece of this project.
        say: has('say'),
        whisper: has(whisperName),
        whisperModel: fsSync.existsSync(whisperModel),
        whisperName,
    };
})();

// Leave a note about which build is *running*. The renderer reads it and warns on the
// panel when the files on disk are newer -- see staleness_banner in render.py.
try {
    fsSync.writeFileSync(
        path.join(path.dirname(fileURLToPath(import.meta.url)), 'loaded-build'), build);
} catch {
    /* not fatal: the banner simply will not appear */
}

export const name = 'mosaico-deck';
// `agents` tells the root conversation from the subagent sessions on the same bus;
// `sessionProjections` is the host's own fold of the session numbers the strip shows.
export const inject = ['tools', 'agents', 'sessionProjections', 'sessionController'];

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
        // Control centre. Levels are the plugin's model of the board's state: the board
        // does not report them back, so they start at the values the firmware boots with
        // (kDefaultVolume and 90/255 brightness).
        lastReply: '',      // opening of the last reply, spoken when the turn ends
        lastSent: '',       // last text sent from the device (chip press or voice)
        chips: [],          // quick-reply hit regions from the last status frame
        question: null,     // a question from the agent, mirrored for the device
        panel: false,
        levels: { volume: 85, brightness: 35 },
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
    let panelHits = [];
    let painting = false;
    // A paint request that arrived while one was in flight. Dropping it silently leaves
    // the panel showing a stale value -- hit squarely by dragging a slider, where the
    // updates come faster than a frame renders. One trailing repaint fixes it.
    let repaintWanted = null;
    // When the (expensive) checkpoint read last ran; see readStats.
    let statsReadAt = 0;
    const STATS_READ_MS = 2000;

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
        if (!deck.connected) return 0;
        if (painting) {
            repaintWanted = { force: force || (repaintWanted?.force ?? false) };
            return 0;
        }
        painting = true;
        try {
            const { rgb, hits } = await render({
                // A picture is drawn as a preview *inside* the status page, not as a
                // full-screen takeover: the state label stays readable above it.
                page: state.question ? 'ask' : (state.panel ? 'panel' : 'status'),
                question: state.question?.question,
                options: state.question?.options,
                levels: state.panel ? state.levels : undefined,
                voice: voiceOn,
                image: state.panel ? null : state.image,
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
                shift: driftShift(),
                replies: state.asking ? [] : quickReplies(),
                // Tap targets exist only while a question is pending. Drawing
                // them otherwise is noise, and a stray tap is expensive.
                buttons: state.asking ? null : undefined,
            });
            // The panel's own tap/drag rectangles, refreshed with every frame so a
            // finger always lands on the geometry that is actually on screen.
            if (state.question) {
                questionHits = hits;
            } else if (state.panel) {
                panelHits = hits;
            } else if (!state.asking) {
                state.chips = hits;
            }
            return deck.sendRgb(rgb, { force });
        } catch (err) {
            // A broken screen must never take the agent down with it.
            ctx.logger?.debug?.(`mosaico-deck: paint failed: ${err.message}`);
            return 0;
        } finally {
            painting = false;
            if (repaintWanted) {
                const next = repaintWanted;
                repaintWanted = null;
                void paint(next);
            }
        }
    }

    // ---- voice ----------------------------------------------------------------
    // The board has had a speaker from the beginning and it was never used: speaking
    // was a tool the model had to remember to call, and it never did. Same lesson as
    // the screen -- **anything that should happen by itself must be driven by host
    // events, not by the model's goodwill.**
    // **Every turn is announced**, at the user's request. The threshold still exists
    // because the tests need it, and because it is the knob if this ever gets noisy.
    const ANNOUNCE_MIN_MS = Number(process.env.DECK_ANNOUNCE_MS || 0);
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
    let warnedNoVoice = false;
    function say(text, { respectGap = false } = {}) {
        const line = String(text ?? '').replace(/\s+/g, ' ').trim();
        if (!line || !voiceOn || !deck.connected) return;
        if (!deps.say || !deps.ffmpeg) {
            // Say why, once: otherwise "the speaker never says anything" is a mystery
            // that looks like a bug in this plugin rather than a missing program.
            if (!warnedNoVoice) {
                warnedNoVoice = true;
                ctx.logger?.warn?.('mosaico-deck: speech disabled — '
                    + `${deps.say ? '' : 'macOS `say` not found '}`
                    + `${deps.ffmpeg ? '' : 'ffmpeg not found '}`
                    + '(run ./check-deps.sh)');
            }
            return;
        }
        if (respectGap && Date.now() - lastSpokeAt < SPEAK_GAP_MS) return;
        speechChain = speechChain.then(async () => {
            try {
                const pcm = await textToPcm(line, state.voiceName);
                lastSpokeAt = Date.now();
                await deck.speakPcm(pcm);
            } catch (err) {
                // **Loud on purpose.** A silent failure here is a feature that looks
                // broken with no evidence anywhere -- which is exactly what happened: the
                // board's audio counter was the only thing that could disprove "it spoke".
                ctx.logger?.warn?.(`mosaico-deck: speech failed: ${err?.message ?? err}`);
            }
        });
    }

    /** The turn is over: settle the label, and say so if it was a long one.
     *
     * **Two independent signals call this**, because one was not enough: the panel sat on
     * 思考中 until the next turn started, which means `agent/status` going idle does not
     * always arrive (and the spoken "完成" hung off the same event, so both symptoms --
     * stuck label, no announcement -- are one cause).
     *
     *   agent/status {status:'idle'}   the loop's own transition
     *   session event `turn/end`       the session log's end-of-turn marker
     *
     * Announcements are gap-guarded, so when both arrive milliseconds apart only one is
     * ever spoken.
     */
    /** What to say when a turn ends: the outcome, not just the word "done". */
    function announcement(elapsed) {
        const mins = Math.floor(elapsed / 60000);
        const head = mins >= 1 ? `完成，用时 ${mins} 分` : '完成';
        // The opening sentence is what tells you whether to come back to the desk.
        const summary = String(state.lastReply ?? '').replace(/\s+/g, ' ').trim();
        const first = summary.split(/[。！？!?]/)[0]?.trim().slice(0, 40) ?? '';
        return first ? `${head}。${first}` : head;
    }

    function endTurn() {
        clearQuestion();
        const elapsed = state.startedAt ? Date.now() - state.startedAt : 0;
        state.status = 'idle';
        state.tool = null;
        // An error was already announced, loudly, when it happened.
        if (state.status !== 'error' && shouldAnnounce({
            voiceOn,
            elapsedMs: elapsed,
            sinceLastMs: Date.now() - lastSpokeAt,
            minMs: ANNOUNCE_MIN_MS,
            minGapMs: SPEAK_GAP_MS,
        })) {
            say(announcement(elapsed), { respectGap: true });
        }
        void paint();
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
        if (!session || !ctx.sessionProjections) return state.stats;

        // **Two read faces, and the cheap one carries the important number.**
        //
        //   snapshot(session, ['contextPressure'])   client-visible units only; cheap
        //   checkpoint(session)                      every registered unit's raw state
        //
        // `tokenUsage` and `sessionStats` are registered **state-only** (no `wire` face),
        // and both `snapshot()` and `viewCheckpoint()` begin with
        // `if (registration.def.wire === void 0) continue;` -- which is why the ring drew
        // while the token totals stayed empty through two attempts at fixing it.
        // `checkpoint()` is the only read face that sees them.
        //
        // It is also the expensive one: it structuredClones every unit, including
        // `turnOutline`, which grows with the conversation. So it is throttled -- these
        // numbers move once per model reply, not once per frame.
        const now = Date.now();
        let rows = null;
        if (now - statsReadAt >= STATS_READ_MS || !state.stats.context) {
            statsReadAt = now;
            try {
                rows = ctx.sessionProjections.checkpoint?.(session) ?? null;
            } catch (err) {
                ctx.logger?.debug?.(`mosaico-deck: checkpoint read failed: ${err.message}`);
            }
        }

        try {
            const pressure = ctx.sessionProjections
                .snapshot(session, ['contextPressure'])?.values?.contextPressure ?? {};
            const totals = rows?.tokenUsage?.val?.totals ?? {};
            const stats = rows?.sessionStats?.val ?? {};
            const decodeMs = numOr0(stats.decodeMs);
            const decodeTokens = numOr0(stats.decodeTokens);
            const uncached = numOr0(totals.uncachedInputTokens);
            const cacheRead = numOr0(totals.cacheReadTokens);
            const read = uncached + cacheRead;
            // **Never zero a number just because this read did not carry it.** A unit
            // that is briefly absent would otherwise blank the whole line.
            const keep = (next, previous) => (next > 0 ? next : (previous ?? 0));
            const next = {
                // Matches the Harness UI exactly (`contextOccupancy`): it prefers
                // projectedTokens and falls back to pressureTokens, and the renderer
                // clamps the ratio to 100 the same way.
                context: keep(numOr0(pressure.projectedTokens ?? pressure.pressureTokens),
                              state.stats.context),
                contextWindow: keep(numOr0(pressure.contextWindow), state.stats.contextWindow),
                input: keep(uncached, state.stats.input),
                output: keep(numOr0(totals.outputTokens), state.stats.output),
                // Share of the prompt that came from cache. Against uncached+cached:
                // dividing by the uncached half alone yields "24700%" once caching works.
                cacheHit: read > 0
                    ? Math.round(100 * cacheRead / read)
                    : (state.stats.cacheHit ?? 0),
                tokensPerSecond: decodeMs > 0
                    ? (decodeTokens / decodeMs) * 1000
                    : (state.stats.tokensPerSecond ?? 0),
                turn: keep(numOr0(stats.turns), state.stats.turn),
                steps: keep(numOr0(stats.steps), state.stats.steps),
            };
            state.stats = next;
            return next;
        } catch {
            return state.stats;   // a projection that is missing is not an error
        }
    }

    function numOr0(v) {
        return Number.isFinite(v) ? v : 0;
    }

    /** Send one user message into the conversation.
     *
     * This is the same path the user's own typing takes (`ctx.sessionController.prompt`,
     * the Host Session API behind the GUI), so a chip press and a voice sentence are
     * **real input**, not a side channel with its own semantics.
     */
    async function sendPrompt(text) {
        const session = rootSession();
        const controller = ctx.sessionController;
        if (!session?.id || typeof controller?.prompt !== 'function') {
            ctx.logger?.warn?.('mosaico-deck: no session controller, prompt dropped');
            pushLog({ kind: 'error', text: 'cannot send' });
            void paint({ force: true });
            return false;
        }
        try {
            await controller.prompt({
                requestId: `deck-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
                sessionId: session.id,
                mode: 'queue',
                content: [{ type: 'text', text }],
                clientTimeZone: Intl.DateTimeFormat().resolvedOptions().timeZone,
            }, new AbortController().signal);
            state.lastSent = text;
            pushLog({ kind: 'reply', text: `sent ${text.slice(0, 16)}` });
            void paint({ force: true });
            return true;
        } catch (err) {
            ctx.logger?.warn?.(`mosaico-deck: prompt failed: ${err?.message ?? err}`);
            pushLog({ kind: 'error', text: 'send failed' });
            void paint({ force: true });
            return false;
        }
    }

    /** Stop the running turn. **Not a message that says "stop"** -- that would queue an
     *  instruction for the model to read after it has already finished, which is the
     *  opposite of stopping it. `cancel` is the Session API's own interrupt. */
    async function cancelTurn() {
        const session = rootSession();
        const controller = ctx.sessionController;
        if (!session?.id || typeof controller?.cancel !== 'function') {
            ctx.logger?.warn?.('mosaico-deck: no session controller, cannot cancel');
            pushLog({ kind: 'error', text: 'no session' });
            void paint({ force: true });
            return false;
        }
        try {
            controller.cancel({ sessionId: session.id });
            pushLog({ kind: 'reply', text: 'cancelled' });
            void paint({ force: true });
            return true;
        } catch (err) {
            // **Visible, not just logged.** "I pressed stop and nothing happened" is
            // unanswerable from the panel if the only trace is a log line.
            ctx.logger?.warn?.(`mosaico-deck: cancel failed: ${err?.message ?? err}`);
            pushLog({ kind: 'error', text: 'stop failed' });
            void paint({ force: true });
            return false;
        }
    }

    /** A question the agent asked, mirrored onto the panel.
     *
     * **Why mirror instead of intercept.** The agent's own question tool is the one that
     * gets used in practice; `deck_ask` is only reached when the model remembers to call
     * it, which is the same "automatic things must not depend on the model" lesson as the
     * voice announcements. So the panel watches for that tool being called and shows the
     * same question.
     *
     * The two surfaces stay **parallel**: answering here sends the choice as an ordinary
     * user message (the path a chip press already takes), so it arrives even while the
     * dialog is still open at the keyboard. It deliberately does *not* block the tool
     * call -- that would break answering at the keyboard, and a device that eats the
     * question is worse than one that only shows it.
     */
    const ASK_TOOL = /^(ask_user_question|ask)$/;
    let questionHits = [];

    function clearQuestion() {
        if (!state.question) return;
        state.question = null;
        questionHits = [];
    }

    function mirrorQuestion(args) {
        const q = (args?.questions ?? [])[0];
        if (!q?.question) return;
        const options = (q.options ?? []).map((o) => String(o?.label ?? o));
        if (options.length === 0) return;
        state.question = { question: String(q.question), options, multi: Boolean(q.multi_select) };
        state.status = 'waiting';
        state.tool = null;
        state.detail = '';
        void paint({ force: true });
    }

    /** Which chips to show right now.
     *
     * **Contextual on purpose.** The user asked "when are these used?", and that question
     * is the panel's job to answer: while a turn is running the only useful thing is to
     * stop it, and while it is idle the useful things are "keep going" and "say more".
     * A fixed row of three would just be three buttons that are wrong most of the time.
     */
    function quickReplies() {
        if (state.asking) return [];
        if (state.status !== 'idle' && state.status !== 'error') {
            return [{ text: '停止本轮', kind: 'cancel' }];
        }
        return [{ text: '继续', kind: 'prompt' }, { text: '详细说说', kind: 'prompt' }];
    }

    /** Push to talk: the firmware streams the microphone up while the button is held. */
    let recording = null;

    /** Hold-to-talk ended: transcribe what was said and send it as this turn's input. */
    async function finishRecording() {
        const pcm = recording;
        recording = null;
        if (!pcm || pcm.length < 4800) {          // < 50 ms: a stray press, not speech
            state.status = 'idle';
            void paint({ force: true });
            return;
        }
        state.status = 'thinking';
        state.detail = '识别中…';
        void paint({ force: true });
        if (!deps.whisper || !deps.whisperModel) {
            const what = !deps.whisper ? `需要 ${deps.whisperName}` : '需要 whisper 模型';
            state.detail = what;
            ctx.logger?.warn?.(`mosaico-deck: push-to-talk unavailable — ${what}`
                + ' (run ./install-deps.sh)');
            void paint({ force: true });
            return;
        }
        try {
            const text = await transcribe(pcm);
            if (text) {
                await sendPrompt(text);
            } else {
                state.detail = '没听清';
                void paint({ force: true });
            }
        } catch (err) {
            ctx.logger?.warn?.(`mosaico-deck: transcription failed: ${err?.message ?? err}`);
            state.detail = '识别失败';
            void paint({ force: true });
        }
    }

    /** 48 kHz stereo from the board -> text, via ffmpeg and whisper.cpp. */
    async function transcribe(pcm) {
        const { execFile } = await import('node:child_process');
        const { promisify } = await import('node:util');
        const run = promisify(execFile);
        const fs = await import('node:fs');
        const os = await import('node:os');
        const nodePath = await import('node:path');
        const dir = fs.mkdtempSync(nodePath.join(os.tmpdir(), 'deck-stt-'));
        try {
            const raw = nodePath.join(dir, 'in.raw');
            const wav = nodePath.join(dir, 'in.wav');
            fs.writeFileSync(raw, pcm);
            // whisper.cpp wants 16 kHz mono; the board sends 48 kHz stereo.
            await run('ffmpeg', ['-y', '-loglevel', 'error', '-f', 's16le', '-ar', '48000',
                '-ac', '2', '-i', raw, '-ar', '16000', '-ac', '1', wav], { timeout: 30000 });
            const bin = process.env.DECK_WHISPER || 'whisper-cli';
            const model = process.env.DECK_WHISPER_MODEL
                || nodePath.join(os.homedir(), '.cache', 'whisper', 'ggml-small.bin');
            const { stdout } = await run(bin, ['-m', model, '-l', 'zh', '-nt', '-f', wav],
                { timeout: 120000, maxBuffer: 8 << 20 });
            return String(stdout).replace(/\s+/g, ' ').trim();
        } finally {
            fs.rmSync(dir, { recursive: true, force: true });
        }
    }

    // ---- burn-in --------------------------------------------------------------
    //
    // This is an always-on AMOLED and the layout barely moves: the header and the stats
    // strip sit in exactly the same place for hours. Static bright content on an emissive
    // panel is what wears it unevenly, and **the damage is permanent** -- so the picture
    // drifts through a small grid of offsets, and the panel dims when nothing is
    // happening. The renderer does the shifting (see its `shift` handling); this decides
    // when and how far.
    const DRIFT_STEP_MS = 2 * 60 * 1000;   // a new offset every two minutes
    const DIM_AFTER_MS = 5 * 60 * 1000;    // idle this long and the panel dims
    const DIM_LEVEL = 8;                   // readable in a dark room, barely emitting
    let lastShift = { x: 0, y: 0 };
    let lastActivityAt = Date.now();
    let dimmed = false;
    let userBrightness = state.levels.brightness;

    /** Where the picture sits right now: a 3x3 grid walked in order, +/-3 px. */
    function driftShift() {
        const step = Math.floor(Date.now() / DRIFT_STEP_MS) % 9;
        return { x: ((step % 3) - 1) * 3, y: (Math.floor(step / 3) - 1) * 3 };
    }

    /** Something happened: wake the panel and restart the idle clock. */
    function markActivity() {
        lastActivityAt = Date.now();
        if (!dimmed) return;
        dimmed = false;
        state.levels.brightness = userBrightness;
        sendLevel('brightness', userBrightness, { force: true });
    }

    /** Same question as isRootSession, for the agent-scoped stream events. */
    function isRootAgent(agent) {
        if (!agent) return true;
        try {
            const roots = ctx.agents?.roots?.();
            if (!Array.isArray(roots) || roots.length === 0) return true;
            return roots.includes(agent);
        } catch {
            return true;
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

    // ---- control centre -------------------------------------------------------
    //
    // Opened by pulling down from the top edge and dragged like a phone's, because that
    // is the interaction everyone already knows for "this thing is too loud / too dim".
    // It only works at all because the firmware now reports `move` events.
    const PULL_EDGE = 60;      // a pull has to start within this many px of the top
    const PULL_DIST = 70;      // ... and travel this far before the panel opens
    // **The panel must never be able to get stuck open**, so there are three ways out:
    // the ✕ button, a vertical swipe in *either* direction, and simply leaving it alone.
    const PANEL_IDLE_CLOSE_MS = Number(process.env.DECK_PANEL_IDLE_CLOSE_MS || 20000);
    let panelTouchedAt = 0;
    let touchStart = null;
    // **The firmware's `up` event carries no coordinates** (`{"t":"touch","ev":"up"}`),
    // so a gesture measured on the release event reads `undefined.y` and never fires.
    // The last position seen on `down`/`move` is what the release is judged against.
    let lastTouch = null;
    let dragging = null;       // 'volume' | 'brightness' while a slider is held
    let lastCtrlAt = 0;

    /** Tell the board. Throttled: a drag produces samples every ~25ms and the board only
     *  needs the value it ends on -- the last one is sent again on release. */
    function sendLevel(key, pct, { force = false } = {}) {
        const now = Date.now();
        if (!force && now - lastCtrlAt < 50) return;
        lastCtrlAt = now;
        deck.sendCtrl(key, pct);
    }

    function setPanel(open) {
        if (state.panel === open) return;
        state.panel = open;
        panelTouchedAt = Date.now();
        panelHits = [];
        dragging = null;
        // force: the panel is a full-page swap, and while the agent is idle nothing else
        // would repaint the old frame away.
        void paint({ force: true });
    }

    function levelFromX(hit, x) {
        const pct = Math.max(0, Math.min(100,
            Math.round((x - hit.x) / hit.w * 100)));
        if (state.levels[dragging] === pct) return;
        state.levels[dragging] = pct;
        if (dragging === 'brightness' && !dimmed) userBrightness = pct;
        sendLevel(dragging, pct);
        void paint();
    }

    deck.on('event', (ev) => {
        if (ev.t === 'touch') {
            markActivity();
            if (ev.ev === 'down') {
                // A mirrored question takes the taps while it is up.
                if (state.question) {
                    const opt = questionHits.find((h) => ev.x >= h.x - 10
                        && ev.x <= h.x + h.w + 10 && ev.y >= h.y - 10
                        && ev.y <= h.y + h.h + 10);
                    if (opt) {
                        const chosen = String(opt.value);
                        clearQuestion();
                        void sendPrompt(chosen);
                        void paint({ force: true });
                    }
                    return;
                }

                // Quick replies first: they are the one control on the status page.
                const chip = !state.panel && !state.asking
                    && state.chips.find((h) => ev.x >= h.x - 6 && ev.x <= h.x + h.w + 6
                        && ev.y >= h.y - 6 && ev.y <= h.y + h.h + 6);
                if (chip) {
                    if (chip.kind === 'cancel') void cancelTurn();
                    else void sendPrompt(String(chip.value));
                    return;
                }
                touchStart = { x: ev.x, y: ev.y };
                lastTouch = { x: ev.x, y: ev.y };
                if (state.panel) {
                    panelTouchedAt = Date.now();
                    // **Toggles and the close button win over the sliders.** Their hit
                    // regions overlap by a few pixels (the voice switch sits just under
                    // the brightness track) and `find` returns whichever comes first, so
                    // a tap on the switch's top edge would adjust brightness instead of
                    // toggling voice.
                    const inside = (h) => ev.x >= h.x - 10 && ev.x <= h.x + h.w + 10
                        && ev.y >= h.y - 10 && ev.y <= h.y + h.h + 10;
                    const hit = panelHits.find((h) => inside(h)
                        && (h.kind === 'toggle' || h.kind === 'close'))
                        ?? panelHits.find(inside);
                    if (hit && hit.kind === 'close') {
                        setPanel(false);
                    } else if (hit && hit.kind === 'toggle') {
                        voiceOn = !voiceOn;
                        pushLog({ kind: 'reply', text: voiceOn ? 'voice on' : 'voice off' });
                        // Confirming by voice when it is switched *on* is the one case
                        // where the feedback can be the thing itself.
                        if (voiceOn) say('语音已开启', { respectGap: false });
                        void paint({ force: true });
                    } else if (hit) {
                        dragging = String(hit.value);
                        levelFromX(hit, ev.x);
                    } else {
                        setPanel(false);       // tapping anywhere else closes it
                    }
                    return;
                }
            } else if (ev.ev === 'move') {
                lastTouch = { x: ev.x, y: ev.y };
                if (state.panel && dragging) {
                    const hit = panelHits.find((h) => String(h.value) === dragging);
                    if (hit) levelFromX(hit, ev.x);
                }
                return;
            } else if (ev.ev === 'up') {
                const from = touchStart;
                const to = lastTouch ?? touchStart;
                const dy = from && to ? to.y - from.y : 0;
                // A vertical gesture is required for either panel gesture: without this a
                // horizontal drag on a slider would also read as a swipe.
                const vertical = from && to && Math.abs(dy) > Math.abs(to.x - from.x);
                if (!state.panel && from && from.y <= PULL_EDGE && dy >= PULL_DIST) {
                    setPanel(true);           // pull down from the top edge to open
                } else if (state.panel && vertical
                    && (dy <= -PULL_DIST || dy >= PULL_DIST)) {
                    // **Swipe either way to close.** Up is what a phone trains for a
                    // control centre; down is what it trains for a sheet. Accepting both
                    // costs nothing and removes a way to get stuck.
                    setPanel(false);
                } else if (dragging) {
                    // The throttled stream may have dropped the final value.
                    sendLevel(dragging, state.levels[dragging], { force: true });
                }
                dragging = null;
                touchStart = null;
                lastTouch = null;
                return;
            }
        }

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

    deck.on('key', (ev) => {
        // Hold-to-talk release: the firmware says so, and the audio has already arrived.
        if (ev?.ev === 'voice_end') {
            markActivity();
            void finishRecording();
            return;
        }
        // Otherwise the physical key cancels a pending question. Easier to find by touch
        // than an on-screen cancel.
        if (!state.asking) return;
        const pending = state.asking;
        state.asking = null;
        buttons = [];
        pending.resolve('cancelled');
    });

    deck.on('audio', (pcm) => {
        if (recording) {
            recording = Buffer.concat([recording, pcm]);
        }
        state.audio = Buffer.concat([state.audio, pcm]);
    });

    // Recording start/stop, as the firmware reports it.
    deck.on('ack', (ev) => {
        if (ev?.rec === undefined) return;
        markActivity();
        if (ev.rec) {
            recording = Buffer.alloc(0);
            state.status = 'listening';
            state.tool = null;
            state.detail = '松手结束';
        }
        void paint({ force: true });
    });

    // -----------------------------------------------------------------------
    // Lifecycle: this is what makes the screen follow the agent
    // -----------------------------------------------------------------------
    ctx.on('agent/status', (payload) => {
        if (payload?.status === 'running') {
            // A new turn: restamp the clock and clear the log ring. **'error' counts as a
            // turn boundary too** -- a turn that ended in a failure leaves the status at
            // 'error', and only checking for 'idle' meant the next turn kept the previous
            // turn's startedAt and reported its duration instead of its own.
            if (state.status === 'idle' || state.status === 'error') {
                state.startedAt = Date.now();
                state.log = [];
                streamTail = '';       // a new turn starts with an empty stream tail
            }
            state.status = 'thinking';
            state.detail = '';   // not 'Working' -- the label already says 思考中
            state.target = '';
            state.tool = null;
        } else {
            endTurn();
            return;
        }
        void paint();
    });

    // waterfall: must forward next()
    ctx.on('tools/pre-execute', (exec, next) => {
        try {
            const { toolName, target } = describeTool(exec);
            state.status = 'tool';
            // Mirror the agent's own questions onto the panel; see mirrorQuestion.
        if (ASK_TOOL.test(toolName)) mirrorQuestion(exec.arguments);
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
        // The session log's own end-of-turn marker: a second, independent "the turn is
        // over" signal. See endTurn().
        if (event?.type === 'turn/start') {
            // **The turn clock is started here, not from `agent/status`.**
            //
            // That was the bug behind "no voice at all": `startedAt` was only set on
            // `agent/status {status:'running'}`, and the panel looked right anyway because
            // `tools/post-execute` also paints 思考中 -- so nothing suggested the event was
            // missing. With `startedAt` stuck at null, `elapsed` was always 0 and the
            // announcement condition could never be true. The evidence that settled it:
            // the board's own `audio playback` counter did not move by a single block
            // across several turns.
            //
            // Session events are the channel that demonstrably works (assistant/message
            // arrives on it), so the turn lifecycle is driven from here.
            if (isRootSession(session)) {
                clearQuestion();
                state.startedAt = Date.now();
                state.log = [];
                state.lastReply = '';
                streamTail = '';
                markActivity();
            }
            return;
        }
        if (event?.type === 'turn/end') {
            if (isRootSession(session)) endTurn();
            return;
        }
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
                state.lastReply = text.replace(/\s+/g, ' ').slice(0, 120);
                state.detail = text.replace(/\s+/g, ' ').slice(0, 34);
                pushLog({ kind: 'reply', text: 'replied' });
            }
        } catch {
            /* ignore */
        }
        void paint();
    });

    // Live output on the panel -- **this is what fills the 思考中 stretch.**
    //
    // A turn is mostly model generation: tools are quick, so before this the detail line
    // sat empty for the whole of it and the panel said 思考中 and nothing else, which is
    // exactly what the user reported. The stream is right there on the bus:
    //
    //     agent/assistant-stream { agent, frame }                       emit
    //     frame   = { type: 'chunk', chunk: StreamChunk, turn, step }
    //     chunk   = { type: 'text-delta' | 'reasoning-delta', text }
    //
    // Reasoning deltas are shown too: when a model exposes its thinking, that answers
    // "what is it doing" far better than the word 思考中.
    const STREAM_PAINT_MS = 250;   // how often the streaming text may repaint the panel
    let streamTail = '';
    let streamPaintedAt = 0;
    let streamTimer = null;
    function paintStream() {
        state.status = 'thinking';
        state.tool = null;
        state.detail = streamTail.replace(/\s+/g, ' ').trim().slice(-60);
        void paint();
    }
    ctx.on('agent/assistant-stream', (payload) => {
        if (!isRootAgent(payload?.agent)) return;
        const chunk = payload?.frame?.chunk;
        if (chunk?.type !== 'text-delta' && chunk?.type !== 'reasoning-delta') return;
        if (typeof chunk.text !== 'string') return;
        streamTail = (streamTail + chunk.text).slice(-400);
        // Chunks arrive per token; the panel cannot and need not keep up with that.
        const now = Date.now();
        const waited = now - streamPaintedAt;
        if (waited >= STREAM_PAINT_MS) {
            streamPaintedAt = now;
            paintStream();
        } else if (streamTimer === null) {
            // **Trailing paint.** Dropping everything inside the window loses the last
            // delta before a pause, so the line sits behind what the model has actually
            // written -- visible as text that stops a few characters short. Same lesson as
            // the repaint coalescing in paint().
            streamTimer = setTimeout(() => {
                streamTimer = null;
                streamPaintedAt = Date.now();
                paintStream();
            }, STREAM_PAINT_MS - waited);
        }
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
        // (the streaming timer is cleared with the rest, below)
        const tickTimer = setInterval(() => {
            if (!deck.connected) return;
            const next = state.status === 'idle'
                ? 0
                : Math.floor((Date.now() - state.startedAt) / 1000);
            // The picture countdown has to tick while the agent is idle too, so it
            // cannot ride on the elapsed-seconds check alone.
            const left = secondsOfImageLeft();
            // The drift moves the whole picture, so it arrives as a full-frame update on
            // its own schedule rather than riding on the seconds counter.
            const drift = driftShift();
            const moved = drift.x !== lastShift.x || drift.y !== lastShift.y;
            if (moved) {
                lastShift = drift;
            }
            // **Only dim while the agent is idle**: the panel exists to be read from
            // across the room, and going dark in the middle of a turn hides the one
            // thing it is for.
            // Safety net: an open panel with no touch for a while closes itself, so it
            // can never sit there blocking the status page.
            if (PANEL_IDLE_CLOSE_MS > 0 && state.panel
                && Date.now() - panelTouchedAt >= PANEL_IDLE_CLOSE_MS) {
                setPanel(false);
                return;
            }
            if (!dimmed && state.status === 'idle'
                && Date.now() - lastActivityAt >= DIM_AFTER_MS) {
                dimmed = true;
                sendLevel('brightness', DIM_LEVEL, { force: true });
            }
            if (next !== state.elapsed || left !== state.imageLeft || moved) {
                state.elapsed = next;
                state.imageLeft = left;
                void paint({ force: false });
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
            const missing = [
                deps.ffmpeg ? '' : 'ffmpeg', deps.say ? '' : 'say(macOS)',
                deps.whisper && deps.whisperModel ? '' : 'whisper',
            ].filter(Boolean);
            const health = missing.length
                ? ` | missing: ${missing.join(', ')} — see ./check-deps.sh`
                : ' | all deps present';
            if (deck.connected) {
                return { message: `already connected to ${deck.port} (build ${build})${health}` };
            }
            if (!connect()) {
                return {
                    message: 'no mosaico-deck data port found: the board must be running '
                        + '(not in ROM download mode) and enumerate as a pair of '
                        + '/dev/cu.usbmodem* nodes; the higher node of the pair is used',
                };
            }
            return { message: `connected to ${deck.port} (build ${build})${health}` };
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

    ctx.tools.register(defineTool({
        name: 'deck_control',
        description:
            'Set the mosaico-deck volume and/or brightness (0-100). Also adjustable on the '
            + 'device itself by pulling down from the top edge, so this is for scripting it '
            + 'rather than for routine use.',
        parameters: {
            volume: { type: 'number', description: '0-100.' },
            brightness: { type: 'number', description: '0-100.' },
        },
        output: textOutput((v) => v.message),
        async execute(args) {
            if (!deck.connected) return { message: 'device not connected' };
            const done = [];
            for (const key of ['volume', 'brightness']) {
                const v = args[key];
                if (v === undefined) continue;
                const pct = Math.max(0, Math.min(100, Math.round(Number(v))));
                state.levels[key] = pct;
                if (key === 'brightness' && !dimmed) userBrightness = pct;
                sendLevel(key, pct, { force: true });
                done.push(`${key}=${pct}`);
            }
            if (done.length === 0) {
                return { message: `volume=${state.levels.volume} brightness=${state.levels.brightness}` };
            }
            void paint();
            return { message: `set ${done.join(' ')}` };
        },
        presentCall: () => ({ card: 'generic', kind: 'read', title: 'Deck volume/brightness' }),
    }));

    ctx.tools.register(defineTool({
        name: 'deck_panel',
        description:
            'Open or close the mosaico-deck control centre (volume and brightness). Mostly '
            + 'useful for showing the user the panel; they can also pull it down from the '
            + 'top edge of the screen.',
        parameters: {
            open: { type: 'boolean', description: 'Omit to toggle.' },
        },
        output: textOutput((v) => v.message),
        async execute(args) {
            if (!deck.connected) return { message: 'device not connected' };
            const open = args.open === undefined ? !state.panel : Boolean(args.open);
            setPanel(open);
            return { message: open ? 'control centre open' : 'control centre closed' };
        },
        presentCall: () => ({ card: 'generic', kind: 'read', title: 'Deck control centre' }),
    }));

    ctx.logger?.info?.(
        `mosaico-deck: build=${build} python=${checkEnvironment().python} `
        + `port=${findPort() ?? 'none'} `
        + `voice=${deps.say && deps.ffmpeg ? 'on' : 'off'} `
        + `speech-in=${deps.whisper && deps.whisperModel ? 'on' : 'off'}`,
    );
}
