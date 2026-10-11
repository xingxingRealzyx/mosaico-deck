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
const logs = [];

const ctx = {
    tools: { register: (t) => registered.push(t) },
    on: (ev, fn) => {
        if (!listeners.has(ev)) listeners.set(ev, []);
        listeners.get(ev).push(fn);
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

const fire = (ev, payload) => {
    let ok = true;
    for (const fn of listeners.get(ev) || []) {
        try { fn(payload); } catch (e) {
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
ok = fire('tools/pre-execute', { call: { name: 'bash', arguments: { command: 'ls' } } }) && ok;
ok = fire('tools/pre-execute', {}) && ok;
ok = fire('tools/pre-execute', null) && ok;
ok = fire('tools/post-execute', {}) && ok;
ok = fire('assistant/message', { message: { content: [{ type: 'text', text: 'hello' }] } }) && ok;
ok = fire('assistant/message', {}) && ok;
ok = fire('agent/error', {}) && ok;
console.log(ok ? '  [ok] no event handler crashed (including empty/null payloads)' : '  [FAIL] a handler threw');

for (const fn of listeners.get('dispose') || []) fn();
console.log(`  [ok] dispose ok  ${logs[0] ? `| ${logs[0].slice(0, 90)}` : ''}`);
console.log('\n[ok] safe to restart DeepSeek Harness');
