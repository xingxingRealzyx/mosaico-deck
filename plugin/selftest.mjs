// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// selftest.mjs -- safely "dry-run" the plugin before restarting
//
// Why it is needed: a profile's bundle is only assembled at startup, so after changing
// the plugin you must restart DSH for it to take effect. And **if the plugin throws while
// loading, DSH itself may fail to start** -- at which point there is no chance to change
// it back. So before restarting, run apply() for real with a simulated cordis ctx.
//
//   # **must be run from the profile directory** (the @deepseek-ai/* modules imported by
//   # the plugin live in ~/.dsh/profiles/node_modules, which cannot be found by walking
//   # up from the source directory)
//   cd ~/.dsh/profiles/desktop && node <repo>/plugin/selftest.mjs
//
//   By default it checks the installed copy; you can also give an explicit path:
//   node selftest.mjs /path/to/lib/index.js
//
// It checks: the module can be imported, it exports name/inject/apply, apply() does not
// throw, the tools register successfully, and the event handlers do not crash even when
// they receive empty/null payloads.

const registered = [];
const listeners = new Map();
const effects = [];
const logs = [];

const ctx = {
    tools: { register: (t) => registered.push(t) },
    on: (ev, fn) => {
        if (!listeners.has(ev)) listeners.set(ev, []);
        listeners.get(ev).push(fn);
    },
    // The real `ctx.effect(body)` runs the body now and disposes whatever it returns
    // when the plugin's fiber goes away. Raw resources must be registered this way:
    // there is no `dispose` event to listen for (see the name guard below).
    effect: (body) => {
        effects.push(body());
    },
    logger: { info: (m) => logs.push(`I ${m}`), debug: (m) => logs.push(`D ${m}`) },
};

// By default check **the installed copy** -- that is the code actually loaded on restart.
const spec = process.argv[2] || 'dsh-mosaico-deck';
let mod;
try {
    mod = await import(spec);
} catch (e) {
    console.log(`  [FAIL] import failed: ${e.message.split('\n')[0]}`);
    process.exit(1);
}
console.log('  [ok] import ok');

if (typeof mod.apply !== 'function' || typeof mod.name !== 'string') {
    console.log('  [FAIL] must export name and apply');
    process.exit(1);
}
console.log(`  [ok] name = ${mod.name}  inject = ${JSON.stringify(mod.inject)}`);

try {
    mod.apply(ctx);
} catch (e) {
    console.log(`  [FAIL] apply() threw: ${e.message}`);
    console.log(e.stack.split('\n').slice(0, 4).join('\n'));
    process.exit(1);
}
console.log('  [ok] apply() returned normally');
console.log(`  [ok] registered ${registered.length} tools: ${registered.map((t) => t.name).join(', ')}`);
console.log(`  [ok] listening to ${listeners.size} events: ${[...listeners.keys()].join(', ')}`);

// Every subscribed name must be a **real bus event**.
//
// This guard exists because the plugin once subscribed to `assistant/message`, which is
// a session event *type* carried by `session/event` -- not an event name. Nothing failed
// here: the handler simply never ran on the host, while this file fired it by name and
// reported success. A name missing from the host's `Events` interfaces is dead code, and
// dead code that only shows up on hardware is the worst kind.
//
// Sources: the `Events` declarations of @deepseek-ai/dsh-agent, dsh-tools and dsh-session.
// Add a name here only after finding it in one of those.
const KNOWN_EVENTS = new Set([
    'agent/created', 'agent/disposed', 'agent/status', 'agent/error',
    'agent/inbox/inserted', 'agent/inbox/claimed', 'agent/inbox/discarded',
    'agent/session-start', 'agent/pre-step', 'agent/request', 'agent/request-error',
    'agent/assistant-stream', 'agent/turn-stopping',
    'tools/change', 'tools/pre-execute', 'tools/execute', 'tools/post-execute',
    'tools/ptc-dispatch-log', 'tools/result',
    'session/created', 'session/disposed', 'session/event', 'session/flush',
]);
const unknown = [...listeners.keys()].filter((e) => !KNOWN_EVENTS.has(e));
if (unknown.length > 0) {
    console.log(`  [FAIL] not bus events: ${unknown.join(', ')} -- find the real name in the host types`);
    process.exit(1);
}
console.log('  [ok] every subscribed name is a real bus event');

const fire = (ev, ...args) => {
    let ok = true;
    for (const fn of listeners.get(ev) || []) {
        try { fn(...args); } catch (e) {
            console.log(`  [FAIL] handler for event ${ev} threw: ${e.message}`);
            ok = false;
        }
    }
    return ok;
};

// **Try both empty and null payloads** -- the host's event shapes are not guaranteed,
// and the plugin must not be picky.
let ok = true;
ok = fire('agent/status', { status: 'running' }) && ok;
ok = fire('agent/status', { status: 'idle' }) && ok;
// The real ToolExecution carries name/arguments at the TOP level, and pre/post are
// waterfalls -- so pass a `next` and make sure the handler forwards it.
ok = fire('tools/pre-execute', { name: 'bash', arguments: { command: 'ls' } }, () => Promise.resolve({ kind: 'allow' })) && ok;
ok = fire('tools/pre-execute', { call: { name: 'bash', arguments: { command: 'ls' } } }) && ok;
ok = fire('tools/pre-execute', {}) && ok;
ok = fire('tools/pre-execute', null) && ok;
ok = fire('tools/post-execute', { name: 'bash', arguments: {} }, {}, () => Promise.resolve({ kind: 'accept' })) && ok;
ok = fire('tools/post-execute', {}) && ok;
// `session/event(session, event)` -- `assistant/message` arrives as an event TYPE.
ok = fire('session/event', null, { type: 'assistant/message', message: { content: [{ type: 'text', text: 'hello' }] } }) && ok;
ok = fire('session/event', null, { type: 'tool/result' }) && ok;
ok = fire('session/event', null, {}) && ok;
ok = fire('session/event') && ok;
ok = fire('agent/error', {}) && ok;
console.log(ok ? '  [ok] no event handler crashed (including empty/null payloads)' : '  [FAIL] a handler threw');

// Cleanup is registered through ctx.effect; if the plugin ever stops doing that, the
// interval and the serial fd leak (which is how a stale process kept the port busy).
if (effects.length === 0) {
    console.log('  [FAIL] no ctx.effect() cleanup registered -- the port would never be released');
    process.exit(1);
}
for (const dispose of effects) {
    if (typeof dispose === 'function') dispose();
}
console.log(`  [ok] effect cleanup ok  ${logs[0] ? `| ${logs[0].slice(0, 90)}` : ''}`);
console.log('\n[ok] safe to restart DeepSeek Harness');
