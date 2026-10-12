// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// test-dead-fd.mjs -- the wedged state the board leaves behind when it re-enumerates.
//
//   node plugin/test-dead-fd.mjs
//
// No hardware needed: a dead fd is simulated with -1, which makes writeSync throw
// EBADF -- the same class of error a reset / replug / flash produces (EBADF, ENXIO).
//
// What matters is that the Deck **stops claiming to be connected**, because
// `connected` is exactly what `deck_connect` consults before deciding whether to
// reopen. If it stays true, the plugin can never recover itself: it keeps rendering
// (one Python process per tool call) and silently drops every frame, which is what
// happened on the real board.

import { Deck } from './lib/device.js';

const deck = new Deck('/dev/null');
deck.fd = -1; // the board went away under us

let threw = false;
let code = '';
try {
    deck._writeAll(Buffer.alloc(8));
} catch (e) {
    threw = true;
    code = e.code;
}

const recovered = deck.connected === false;
console.log(`  [${threw ? 'ok' : 'FAIL'}] writing to a dead fd throws (${code || 'nothing thrown'})`);
console.log(`  [${recovered ? 'ok' : 'FAIL'}] deck no longer claims to be connected`);

const pass = threw && recovered;
console.log(pass
    ? '\n[ok] a dead fd is recoverable: deck_connect can reopen the port'
    : '\n[FAIL] the deck would stay wedged -- connected=true forever, every frame dropped');
process.exit(pass ? 0 : 1);
