// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// render.js —— 调 render.py 画一页
//
// **用 DSH 自带的那份 Python。**
// `runtime/primary-runtime/dependencies/python` 里有 Python 3.12 + Pillow，
// 而 DSH 自己就是靠它跑 Office 技能的 —— 所以这不是"额外装个 Python"，
// 是复用宿主已经带的运行时。
//
// 为什么不自带渲染：Node 侧没法用原生模块（见 device.js 顶部的说明），
// 纯 JS 画中文要自己做字形栅格化。而 Pillow 这条链已经在跑、也验证过了。

import { execFile } from 'node:child_process';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SCRIPT = path.join(HERE, '..', 'render.py');

const CANDIDATES = [
    // DSH 打包进去的那份（首选：一定有 Pillow）
    process.env.DSH_PYTHON,
    '/Applications/DeepSeek Harness.app/Contents/Resources/runtime/primary-runtime/dependencies/python/bin/python3',
    // 开发时的虚拟环境
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
 * 渲染一页。
 *
 * 输出 **裸 RGB**（480×480×3）而不是 PNG：调用方拿去做 band 差分，本来就要
 * 逐字节比对整幅图，PNG 还得先解压。
 *
 * @param {object} job 页面描述，形如 { page: 'status', state, detail, ... }
 * @returns {Promise<{rgb: Buffer, hits: Array}>}
 */
export function render(job) {
    const needHits = job.hits !== false;
    return new Promise((resolve, reject) => {
        const child = execFile(
            pythonPath(),
            [SCRIPT],
            // maxBuffer 别写 0 —— 在 Node 里那是"最多 0 字节"，会直接报
            // 'stderr maxBuffer length exceeded'，而不是"不限制"。
            { maxBuffer: 32 * 1024 * 1024, encoding: 'buffer', timeout: 15000 },
            (err, stdout, stderr) => {
                if (err) {
                    reject(new Error(`渲染失败: ${err.message}`));
                    return;
                }
                if (stdout.length !== PAGE_BYTES) {
                    reject(new Error(`渲染输出 ${stdout.length} 字节，期望 ${PAGE_BYTES}`));
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

/** 综合环境检查，给插件一个"能不能用"的快速答案。 */
export function checkEnvironment() {
    const py = pythonPath();
    return { python: py, script: SCRIPT, exists: fs.existsSync(SCRIPT) };
}
