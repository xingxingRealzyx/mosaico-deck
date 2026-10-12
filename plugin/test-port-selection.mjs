// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// test-port-selection.mjs -- the one rule that cannot be checked by looking at code.
//
// Picking the wrong node here does not fail loudly: it opens the *flashing* port,
// asserts DTR/RTS on it, and kicks the chip out of ROM download mode -- the exact
// failure the two-port CDC split exists to prevent. So the rule is a pure function
// (pickDataPort in lib/device.js) and this file pins it down.
//
//   node plugin/test-port-selection.mjs
//
// No hardware and no dependencies: it only exercises the choice.

import { pickDataPort } from './lib/device.js';

// The pair as NEXT-STEPS.md describes it: port 0 (flashing) is the lower suffix,
// port 1 (data) is the higher one.
const CASES = [
    {
        what: 'no board at all',
        names: [],
        want: null,
    },
    {
        what: 'single node -- ROM download mode, or a stale single-CDC firmware',
        names: ['cu.usbmodem21201'],
        want: null,
    },
    {
        what: 'the documented pair -> the higher (data) node',
        names: ['cu.usbmodem21201', 'cu.usbmodem21203'],
        want: '/dev/cu.usbmodem21203',
    },
    {
        what: 'same pair, listed in reverse -- order must not matter',
        names: ['cu.usbmodem21203', 'cu.usbmodem21201'],
        want: '/dev/cu.usbmodem21203',
    },
    {
        what: 'the pair plus an unrelated single-node device',
        names: ['cu.usbmodem21201', 'cu.usbmodem21203', 'cu.usbmodem31101'],
        want: '/dev/cu.usbmodem21203',
    },
    {
        what: 'one unrelated single-node device alone -> still nothing',
        names: ['cu.usbmodem31101'],
        want: null,
    },
    {
        what: 'two boards, both dual-CDC -> deterministic (first pair in name order)',
        names: ['cu.usbmodem21201', 'cu.usbmodem21203', 'cu.usbmodem31101', 'cu.usbmodem31103'],
        want: '/dev/cu.usbmodem21203',
    },
];

let failed = 0;
for (const { what, names, want } of CASES) {
    const got = pickDataPort(names);
    const pass = got === want;
    if (!pass) failed++;
    console.log(`  [${pass ? 'ok' : 'FAIL'}] ${what}`);
    console.log(`         [${names.join(', ')}] -> ${got}${pass ? '' : `  (expected ${want})`}`);
}

console.log(failed === 0
    ? '\n[ok] port selection matches the dual-CDC contract'
    : `\n[FAIL] ${failed} case(s) wrong -- do NOT flash with this plugin loaded`);
process.exit(failed === 0 ? 0 : 1);
