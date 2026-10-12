// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// test-plugin-events.mjs -- what the plugin DOES with host events, without a board.
//
//   node plugin/test-plugin-events.mjs
//
// The renderer has its own test (test_render_layout.py) and the port choice has its own
// (test-port-selection.mjs). What was missing is the middle: the mapping from host
// events to the job handed to the renderer. That mapping is where "reading an image
// puts the picture on the panel" lives, and it cannot be verified by looking at the
// panel -- only by watching what the plugin actually asks the renderer to draw.
//
// Both the device and the render layers are **stubbed** in a throwaway copy, so this
// never opens a serial port and never spends a Python process: lib/device.js records
// that a frame was sent, lib/render.js records the job it was given.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DSH_MODULES = path.join(os.homedir(), '.dsh', 'profiles', 'node_modules', '@deepseek-ai');

if (!fs.existsSync(DSH_MODULES)) {
    console.log(`  [skip] ${DSH_MODULES} not found -- run this where DeepSeek Harness is installed`);
    process.exit(0);
}

// Watch the picture expiry for real, in milliseconds rather than ten seconds.
process.env.DECK_IMAGE_HOLD_MS = '300';

// ---------------------------------------------------------------- scratch copy
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-events-'));
fs.mkdirSync(path.join(root, 'lib'), { recursive: true });
fs.mkdirSync(path.join(root, 'node_modules'), { recursive: true });
fs.symlinkSync(DSH_MODULES, path.join(root, 'node_modules', '@deepseek-ai'));
fs.copyFileSync(path.join(HERE, 'lib', 'index.js'), path.join(root, 'lib', 'index.js'));
fs.copyFileSync(path.join(HERE, 'package.json'), path.join(root, 'package.json'));

fs.writeFileSync(path.join(root, 'lib', 'device.js'), `
export const BAND_COUNT = 16;
export function findPort() { return '/dev/stub'; }
export class Deck {
    constructor() { this.fd = 1; this.port = '/dev/stub'; }
    get connected() { return true; }
    open() { return true; }
    close() {}
    on() { return this; }
    sendRgb() { globalThis.__frames = (globalThis.__frames || 0) + 1; return 1; }
}
`);

fs.writeFileSync(path.join(root, 'lib', 'render.js'), `
export const PAGE_BYTES = 480 * 480 * 3;
export function checkEnvironment() { return { python: 'stub', script: 'stub', exists: true }; }
export function render(job) {
    globalThis.__jobs = globalThis.__jobs || [];
    globalThis.__jobs.push(JSON.parse(JSON.stringify(job)));
    return Promise.resolve({ rgb: Buffer.alloc(PAGE_BYTES), hits: [] });
}
`);

// ---------------------------------------------------------------- drive it
const listeners = new Map();
const registered = [];
const effects = [];
const ctx = {
    tools: { register: (t) => registered.push(t) },
    on: (ev, fn) => {
        if (!listeners.has(ev)) listeners.set(ev, []);
        listeners.get(ev).push(fn);
    },
    // Keep the disposer, exactly like the real host does: dropping it here leaves the
    // plugin's tick interval running and this process never exits.
    effect: (body) => { effects.push(body()); },
    logger: { info() {}, debug() {} },
};

const mod = await import(pathToFileURL(path.join(root, 'lib', 'index.js')).href);
mod.apply(ctx);

let failures = 0;
const check = (ok, msg) => {
    if (!ok) failures++;
    console.log(`  [${ok ? 'ok' : 'FAIL'}] ${msg}`);
};
const fire = async (ev, ...args) => {
    for (const fn of listeners.get(ev) || []) fn(...args);
    await new Promise((r) => setTimeout(r, 40));   // let paint() settle
};
const job = () => (globalThis.__jobs || []).at(-1) || {};
const pre = (name, args) => fire('tools/pre-execute', { name, arguments: args }, () => {});
const reply = (text) => fire('session/event', null,
    { type: 'assistant/message', message: { content: [{ type: 'text', text }] } });

console.log('构建指纹');
check(typeof mod.build === 'string' && mod.build.length === 7,
      `导出 build 指纹：${mod.build}（用来判断进程里跑的是哪份代码）`);

console.log('注册的工具');
check(registered.length === 5, `5 个工具：${registered.map((t) => t.name).join(', ')}`);
check(registered.some((t) => t.name === 'deck_image'), 'deck_image 已注册');

console.log('工具调用 -> 渲染 job');
await pre('bash', { command: 'pytest -q' });
check(job().page === 'status' && job().tool === 'bash', 'bash 走状态页并带上工具名');
check(job().log?.[0]?.kind === 'tool' && job().log?.[0]?.tool === 'bash',
    '日志条目是结构化的（渲染器据此选 emoji）');
check(job().target === 'pytest -q', 'target 取自命令');

console.log('读图片 -> 状态页上的图片预览');
await pre('read_image', { file_path: '/tmp/deck-demo.png' });
check(job().image === '/tmp/deck-demo.png', 'image 是那张图的绝对路径');
check(job().page === 'status',
    '图片画在状态页里（保留状态头），不是整屏接管');
check(job().imageLeft > 0, `带倒计时：${job().imageLeft}s 后自动清除`);

await reply('这张图里有个 bug');
check(job().image === '/tmp/deck-demo.png',
    '助手回复**不**清掉图片（用户正是在这时要看着图读解释）');

await pre('read', { file_path: '/tmp/notes.py' });
check(job().image === null, '读非图片文件把图撤下');

await pre('write', { file_path: '/tmp/out.png' });
check(job().image === null, '写入 .png 不会把图弹上屏（只认读）');

console.log('图片会自动清除（不等下一次工具调用）');
await pre('read_image', { file_path: '/tmp/b.png' });
check(job().image === '/tmp/b.png', '先显示出来');
await new Promise((r) => setTimeout(r, Number(process.env.DECK_IMAGE_HOLD_MS) + 250));
check(job().image === null && job().page === 'status',
    `${Number(process.env.DECK_IMAGE_HOLD_MS)}ms 后自己消失并回到状态页`);

await pre('read_image', { file_path: '/tmp/a.png' });
await pre('bash', { command: 'echo hi' });
check(job().image === null, '读图之后的下一次普通工具调用也会撤图');

console.log('相对路径与边界');
await pre('read_image', { file_path: 'shots/a.png' });
check(path.isAbsolute(job().image || ''), `相对路径被解析成绝对路径：${job().image}`);

await fire('tools/pre-execute', {}, () => {});
check(job().page === 'status', '空载荷不崩、退回状态页');

await fire('tools/pre-execute', null, () => {});
check(job().page === 'status', 'null 载荷不崩');

console.log('detail 不允许只是状态词的同义重复');
await pre('bash', { command: 'ls -la' });
check(job().detail === 'Calling bash', '工具进行中 detail 是会变的工具名（有用）');
await fire('tools/post-execute', { name: 'bash', arguments: {} }, {}, () => {});
check(job().state === 'thinking' && !job().detail,
    `工具结束后不写死 "Tool finished"（现在 detail=${JSON.stringify(job().detail)}）`);

console.log('状态事件');
await fire('agent/status', { status: 'running' });
check(job().state === 'thinking' && !job().detail,
    `agent/status running -> 思考中，且不写死 "Working"（detail=${JSON.stringify(job().detail)}）`);
await fire('agent/status', { status: 'idle' });
check(job().state === 'idle', 'agent/status idle -> idle');

console.log('出错时把真实错误写到面板上（而不是写死的 "Error"）');
await fire('agent/error', { error: new Error('connect ECONNREFUSED 127.0.0.1:19387') });
check(job().state === 'error' && job().log?.at(-1)?.kind === 'error',
    'agent/error -> error 状态 + 结构化错误日志');
check(job().detail.includes('ECONNREFUSED'), `detail 带真实信息：${job().detail}`);
await fire('agent/error', { error: { message: '普通对象形式的失败' } });
check(job().detail === '普通对象形式的失败', '对象形式的错误也能读出 message');
await fire('agent/error', { error: '纯字符串失败' });
check(job().detail === '纯字符串失败', '字符串形式的错误原样用');
await fire('agent/error', {});
check(job().detail === '', '没有错误信息时 detail 为空，而不是写死 "Error"');
const circular = {};
circular.self = circular;
await fire('agent/error', { error: circular });
check(typeof job().detail === 'string', '循环引用的错误对象不会让处理函数抛异常');
await fire('agent/error', { error: new Error('x'.repeat(200)) });
check(job().detail.length <= 40, `超长错误信息被截断到 ${job().detail.length} 字`);

for (const dispose of effects) {
    if (typeof dispose === 'function') dispose();
}
fs.rmSync(root, { recursive: true, force: true });
console.log();
if (failures) {
    console.log(`[FAIL] ${failures} 项未通过`);
    process.exit(1);
}
console.log('[ok] 宿主事件到渲染 job 的映射符合契约');
process.exit(0);
