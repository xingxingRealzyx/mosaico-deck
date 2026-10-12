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
process.env.DECK_ANNOUNCE_MS = '0';     // 让"回合结束播报"立即满足条件
process.env.DECK_SPEAK_GAP_MS = '0';    // 也不要因为刚说过话而被间隔挡住

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
    on(ev, fn) { (this._h = this._h || {})[ev] = ((this._h || {})[ev] || []).concat(fn); globalThis.__deck = this; return this; }
    emit(ev, ...a) { for (const f of ((this._h || {})[ev] || [])) f(...a); }
    sendRgb() { globalThis.__frames = (globalThis.__frames || 0) + 1; return 1; }
    sendCtrl(cmd, value) { (globalThis.__ctrl = globalThis.__ctrl || []).push([cmd, value]); return true; }
}
`);

fs.writeFileSync(path.join(root, 'lib', 'tts.js'), `
export async function textToPcm(text) {
    globalThis.__spoken = globalThis.__spoken || [];
    globalThis.__spoken.push(text);
    return Buffer.alloc(4800);          // 50ms 的假 PCM，不用真合成
}
`);

fs.writeFileSync(path.join(root, 'lib', 'render.js'), `
export const PAGE_BYTES = 480 * 480 * 3;
export function checkEnvironment() { return { python: 'stub', script: 'stub', exists: true }; }
export function render(job) {
    globalThis.__jobs = globalThis.__jobs || [];
    globalThis.__jobs.push(JSON.parse(JSON.stringify(job)));
    // The touch handler hit-tests whatever the renderer last reported, so the stub has to
    // return the panel's real rows -- an empty list makes the panel untouchable here.
    // render.py owns the geometry and plugin/test_render_layout.py pins these numbers.
    const hits = job.page === 'panel'
        ? [{ x: 20, y: 160, w: 440, h: 54, value: 'volume' },
           { x: 20, y: 300, w: 440, h: 54, value: 'brightness' }]
        : [];
    return Promise.resolve({ rgb: Buffer.alloc(PAGE_BYTES), hits });
}
`);

// ---------------------------------------------------------------- drive it
const listeners = new Map();
const registered = [];
const effects = [];
// The root conversation vs a subagent's session. Usage from the first must be counted
// and from the second ignored, so the mock registry has to tell them apart.
const ROOT_SESSION = { who: 'root' };
const SUBAGENT_SESSION = { who: 'subagent' };
const ctx = {
    tools: { register: (t) => registered.push(t) },
    agents: { roots: () => [{ session: ROOT_SESSION }] },
    // The host folds the session numbers itself; the plugin only reads them. Real
    // projection names, with values shaped like the real ones.
    // Mirrors the host: `snapshot` exposes only client-visible units (contextPressure has
    // a wire face), while tokenUsage / sessionStats are state-only and are reachable
    // through the checkpoint read face. Getting this wrong is what left the strip showing
    // a ring and nothing else.
    sessionProjections: {
        snapshot: (session) => ({
            asOfSeq: 1,
            values: session === ROOT_SESSION
                ? { contextPressure: { pressureTokens: 625830, contextWindow: 1000000,
                                       surfaceTokens: 481943 } }
                : {},
        }),
        // Real semantics: raw state per key, wrapped as {ver, seq, val} -- and this is
        // the ONLY read face that sees state-only units.
        checkpoint: (session) => (session === ROOT_SESSION ? {
            contextPressure: { ver: 5, seq: 1, val: { pressureTokens: 625830,
                                                       contextWindow: 1000000 } },
            tokenUsage: { ver: 2, seq: 1, val: { totals: {
                uncachedInputTokens: 574196, outputTokens: 398731,
                cacheReadTokens: 141789056, cacheWriteTokens: 0 } } },
            sessionStats: { ver: 1, seq: 1, val: { turns: 16, steps: 389, llmMs: 2693785,
                                                   toolMs: 4616652, ttftMs: 760550,
                                                   decodeMs: 1933235, decodeTokens: 398731 } },
        } : {}),
        viewCheckpoint: () => ({}),   // 只给有 wire 的单元：这里什么都不给（真实行为）
    },
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
const reply = (text) => fire('session/event', ROOT_SESSION,
    { type: 'assistant/message', message: { content: [{ type: 'text', text }] } });

console.log('构建指纹');
check(typeof mod.build === 'string' && mod.build.length === 7,
      `导出 build 指纹：${mod.build}（用来判断进程里跑的是哪份代码）`);

console.log('注册的工具');
check(registered.length === 8,
    `8 个工具：${registered.map((t) => t.name).join(', ')}`);
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

console.log('会话数字：直接读宿主的投影，不自己累加');
// 真实值取自本机的投影缓存，形状与宿主一致。
await pre('bash', { command: 'ls' });        // 任意一次 paint 都会带上 stats
let st = job().stats;
check(st?.context === 625830, `上下文压力来自 contextPressure.pressureTokens：${st?.context}`);
check(st?.contextWindow === 1000000, `窗口来自 contextPressure.contextWindow：${st?.contextWindow}`);
check(st?.input === 574196 && st?.output === 398731,
    `累计 token 来自 tokenUsage.totals：${st?.input}/${st?.output}`);
check(st?.cacheHit === 100,
    `缓存命中率 = cacheRead/(uncached+cacheRead)：${st?.cacheHit}%`
    + `（真实值 141789056/(574196+141789056) 也是 100%）`);
check(st?.turn === 16 && st?.steps === 389, `轮次/步数来自 sessionStats：${st?.turn}轮 ${st?.steps}步`);
check(Math.abs((st?.tokensPerSecond ?? 0) - 206.25) < 0.1,
    `解码速度 = decodeTokens/decodeMs，与 Harness 同口径：${st?.tokensPerSecond?.toFixed(1)} tok/s`);

// 这条是**回归测试**：以前是自己从 assistant/message.usage 累加，而那个字段到不了
// 监听器，于是底部一直空着。现在数字只有一个口径——投影。
await fire('session/event', ROOT_SESSION,
    { type: 'assistant/message', turn: 99, step: 9, usage: { inputTokens: 5, outputTokens: 5 } });
await pre('bash', { command: 'echo x' });
check(job().stats?.turn === 16 && job().stats?.input === 574196,
    'assistant/message 的字段不再影响数字（口径只有投影一个）');
check(job().stats?.steps === 389, '自己累加的步数已彻底移除，不再和宿主打架');

// 子代理的会话读同一组投影名，但插件只认根会话。
await fire('session/event', SUBAGENT_SESSION, { type: 'assistant/message', turn: 1, step: 1 });
await pre('bash', { command: 'echo y' });
check(job().stats?.turn === 16, '根会话仍由 agents.roots() 决定，子代理不会替换它');

// 投影服务不可用时必须降级，而不是把面板搞崩。
const snapshotFn = ctx.sessionProjections.snapshot;
const checkpointFn = ctx.sessionProjections.checkpoint;
ctx.sessionProjections.snapshot = () => { throw new Error('projection offline'); };
ctx.sessionProjections.checkpoint = () => { throw new Error('projection offline'); };
await pre('bash', { command: 'echo z' });
check(job().page === 'status' && job().stats?.context === 625830,
    `投影读取抛异常时不崩、也**不清零**，保留上次读数（context 仍是 ${job().stats?.context}）`);
check(job().stats?.cacheHit === 100, '缓存命中同样保留，不会因为一次读失败就消失');
ctx.sessionProjections.snapshot = snapshotFn;
ctx.sessionProjections.checkpoint = checkpointFn;

console.log('语音：关键节点自动出声（不再等模型主动想起来调 deck_speak）');
check(mod.shouldAnnounce({ voiceOn: true, elapsedMs: 31000, sinceLastMs: 99999,
                           minMs: 30000, minGapMs: 4000 }),
    '长回合该播报');
check(!mod.shouldAnnounce({ voiceOn: true, elapsedMs: 3000, sinceLastMs: 99999,
                            minMs: 30000, minGapMs: 4000 }),
    '短回合不播报 —— 3 秒的来回不值得打断房间');
check(!mod.shouldAnnounce({ voiceOn: false, elapsedMs: 60000, sinceLastMs: 99999,
                            minMs: 30000, minGapMs: 4000 }),
    '静音时一律不播报');
check(!mod.shouldAnnounce({ voiceOn: true, elapsedMs: 60000, sinceLastMs: 100,
                            minMs: 30000, minGapMs: 4000 }),
    '刚说过话时不连着说');

globalThis.__spoken = [];
await fire('agent/error', { error: new Error('连接被拒绝') });
await new Promise((r) => setTimeout(r, 80));
check((globalThis.__spoken || []).some((t) => t.includes('出错') && t.includes('连接被拒绝')),
    `出错会念出来：${JSON.stringify(globalThis.__spoken)}`);

globalThis.__spoken = [];
await fire('agent/status', { status: 'running' });
await new Promise((r) => setTimeout(r, 60));   // 让 startedAt 落在过去
await fire('agent/status', { status: 'idle' });
await new Promise((r) => setTimeout(r, 80));
check((globalThis.__spoken || []).length === 1,
    `足够长的回合结束会播报：${JSON.stringify(globalThis.__spoken)}`);

globalThis.__spoken = [];
await fire('tools/pre-execute', { name: 'bash', arguments: { command: 'ls' } }, () => {});
await new Promise((r) => setTimeout(r, 60));
check((globalThis.__spoken || []).length === 0,
    '普通工具调用不会出声（否则就是噪音）');

console.log('deck_voice 可以静音');
check(registered.some((t) => t.name === 'deck_voice'), 'deck_voice 已注册');
globalThis.__spoken = [];
await fire('agent/error', { error: new Error('静音时的失败') });
await new Promise((r) => setTimeout(r, 80));
check((globalThis.__spoken || []).length === 1, '静音前先确认它能说话');
const voiceTool = registered.find((t) => t.name === 'deck_voice');
await voiceTool.execute({ enabled: false });
globalThis.__spoken = [];
await fire('agent/error', { error: new Error('静音后的失败') });
await new Promise((r) => setTimeout(r, 80));
check((globalThis.__spoken || []).length === 0, '关掉之后不再出声');
await voiceTool.execute({ enabled: true });

console.log('控制中心：顶部下拉打开 / 拖动滑块 / 点空白关闭');
// **`up` carries no coordinates** -- that is the real firmware payload, and reading
// `ev.y` off it is why the pull-down never fired.
const touch = (ev, x, y) => globalThis.__deck.emit('event',
    ev === 'up' ? { t: 'touch', ev } : { t: 'touch', ev, x, y });
touch('down', 200, 30);
touch('move', 200, 70);      // 真机上手指是连续移动的，move 会一路跟到抬手处
touch('move', 200, 150);
touch('up');
await new Promise((r) => setTimeout(r, 60));
check(job().page === 'panel', `从顶部下拉会打开控制中心（page=${job().page}）`);
check(job().levels?.volume === 85, `面板带着当前档位：音量 ${job().levels?.volume}%`);

// 滑块之外的点按是"关闭"，所以拖拽测试必须紧接着打开状态进行。

globalThis.__ctrl = [];
touch('down', 100, 190);          // 音量滑块那一行
touch('move', 240, 190);          // 拖到中间
touch('up');
await new Promise((r) => setTimeout(r, 60));
const vol = job().levels?.volume;
check(vol >= 45 && vol <= 55, `拖动滑块把音量调到 ${vol}%`);
check((globalThis.__ctrl || []).some(([c, v]) => c === 'volume' && Math.abs(v - vol) <= 6),
    `命令也发给了板子：${JSON.stringify(globalThis.__ctrl)}`);

globalThis.__ctrl = [];
touch('down', 240, 430);          // 滑块之外
await new Promise((r) => setTimeout(r, 60));
check(job().page === 'status', '点空白处关闭面板');

// 只有 down + up、中间没有 move 时不能算下拉（否则一次顶部轻点就会弹出面板）
globalThis.__deck.emit('event', { t: 'touch', ev: 'down', x: 200, y: 20 });
globalThis.__deck.emit('event', { t: 'touch', ev: 'up' });
await new Promise((r) => setTimeout(r, 60));
check(job().page === 'status', '顶部轻点（没有拖动）不会弹出面板');

// 手机的习惯是上滑关闭。从**滑块上**上滑才测得出这个手势：从空白处上滑在按下那一刻
// 就已经关闭了，测过了也是白测。
touch('down', 200, 30);
touch('move', 200, 150);
touch('up');
await new Promise((r) => setTimeout(r, 60));
check(job().page === 'panel', '重新打开面板以便测试上滑');

touch('down', 240, 190);          // 手落在音量滑块上
touch('move', 240, 140);
touch('move', 240, 100);
touch('up');
await new Promise((r) => setTimeout(r, 60));
check(job().page === 'status', '从滑块上上滑也能关闭面板');

// 横向拖动滑块不能被误判成上滑
touch('down', 200, 30);
touch('move', 200, 150);
touch('up');
await new Promise((r) => setTimeout(r, 60));
touch('down', 100, 190);
touch('move', 240, 190);
touch('up');
await new Promise((r) => setTimeout(r, 60));
check(job().page === 'panel', '横向拖动滑块不会误关闭面板');
check(job().levels?.volume >= 45 && job().levels?.volume <= 55,
    `而且音量确实改了：${job().levels?.volume}%`);
touch('down', 240, 430);          // 收尾：关掉
await new Promise((r) => setTimeout(r, 60));

console.log('deck_control / deck_panel');
check(registered.some((t) => t.name === 'deck_control'), 'deck_control 已注册');
check(registered.some((t) => t.name === 'deck_panel'), 'deck_panel 已注册');
const ctrlTool = registered.find((t) => t.name === 'deck_control');
globalThis.__ctrl = [];
const res = await ctrlTool.execute({ brightness: 80 });
check((globalThis.__ctrl || []).some(([c, v]) => c === 'brightness' && v === 80),
    `deck_control 直接改亮度并发命令：${res.message}`);
check((await ctrlTool.execute({})).message.includes('brightness=80'),
    '不带参数时报告当前档位');

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
