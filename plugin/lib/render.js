// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// render.js -- calls render.py to draw one page
//
// **Use the Python that ships with DSH.**
// `runtime/primary-runtime/dependencies/python` has Python 3.12 + Pillow,
// and DSH itself relies on it to run the Office skills -- so this is not
// "installing an extra Python", it is reusing a runtime the host already ships.
//
// Why not bundle our own renderer: the Node side cannot use native modules (see the
// note at the top of device.js), and drawing Chinese text in pure JS would mean doing
// glyph rasterization ourselves. The Pillow chain is already running and verified.

import { execFile } from 'node:child_process';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SCRIPT = path.join(HERE, '..', 'render.py');

const CANDIDATES = [
    // the one bundled with DSH (first choice: Pillow is guaranteed to be there)
    process.env.DSH_PYTHON,
    '/Applications/DeepSeek Harness.app/Contents/Resources/runtime/primary-runtime/dependencies/python/bin/python3',
    // the virtualenv used during development
    path.join(HERE, '..', '..', '.venv', 'bin', 'python'),
    'python3',
].filter(Boolean);

export const PAGE_BYTES = 480 * 480 * 3;

let cachedPython = null;

function pythonPath() {
    if (cachedPython) return cachedPython;
    for (const p of CANDIDATES) {
        if (p.includes('/') && !fs.existsSync(p)) continue;
        cachedPython = p;
        return p;
    }
    cachedPython = 'python3';
    return cachedPython;
}

/**
 * Render one page.
 *
 * The output is **raw RGB** (480x480x3) rather than PNG: the caller uses it for band
 * diffing and has to compare the whole image byte by byte anyway, while PNG would have
 * to be decompressed first.
 *
 * @param {object} job page description, shaped like { page: 'status', state, detail, ... }
 * @returns {Promise<{rgb: Buffer, hits: Array}>}
 */
export function render(job) {
    const needHits = job.hits !== false;
    return new Promise((resolve, reject) => {
        const child = execFile(
            pythonPath(),
            [SCRIPT],
            // Do not set maxBuffer to 0 -- in Node that means "at most 0 bytes" and it
            // reports 'stderr maxBuffer length exceeded' instead of "unlimited".
            { maxBuffer: 32 * 1024 * 1024, encoding: 'buffer', timeout: 15000 },
            (err, stdout, stderr) => {
                if (err) {
                    reject(new Error(`render failed: ${err.message}`));
                    return;
                }
                if (stdout.length !== PAGE_BYTES) {
                    reject(new Error(`render produced ${stdout.length} bytes, expected ${PAGE_BYTES}`));
                    return;
                }
                let hits = [];
                if (needHits && stderr && stderr.length) {
                    try {
                        hits = JSON.parse(stderr.toString('utf8').trim().split('\n').pop());
                    } catch {
                        hits = [];
                    }
                }
                resolve({ rgb: stdout, hits });
            },
        );
        if (child.stdin) {
            child.stdin.end(JSON.stringify({ hits: needHits, ...job }));
        }
    });
}

/** Combined environment check, giving the plugin a quick "can this work" answer. */
export function checkEnvironment() {
    const py = pythonPath();
    return { python: py, script: SCRIPT, exists: fs.existsSync(SCRIPT) };
}
