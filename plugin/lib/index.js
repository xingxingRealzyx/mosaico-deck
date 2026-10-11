// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// index.js —— mosaico-deck 的 DeepSeek Harness 原生插件
//
// 它做两件 MCP 做不到的事
// ---------------------
// **1. 界面自动跟着 agent 走，模型一个字都不用说。**
//    挂上 `agent/status`、`tools/pre-execute`、`assistant/message`，屏幕上就
//    自动出现"思考中 / 正在跑 bash / 说什么"—— 模型完全不需要记得调工具。
//    MCP 工具做不到这一点：模型得主动想起来调，而它经常想不起来。
//
// **2. 状态是宿主事件驱动的，不是模型描述的。**
//    模型说"我在跑测试"可能是在编；而 `tools/pre-execute` 里拿到的是真的
//    工具名和参数。设备上显示的因此是**事实**，不是模型的自我报告。
//
// 三个工具是补充，不是主力：speak（说话）、ask（问人）、deck_show（手工改）。

import { defineTool } from '@deepseek-ai/dsh-tools';

import { Deck, findPort } from './device.js';
import { render, checkEnvironment } from './render.js';

export const name = 'mosaico-deck';
export const inject = ['tools'];

const RECONNECT_MS = 5000;
const TICK_MS = 1000; // 计时器走秒，用来刷新顶部时间

export function apply(ctx) {
    const state = {
        connected: false,
        project: 'Harness',
        status: 'idle',
        detail: '',
        target: '',
        progress: null,
        elapsed: 0,
        startedAt: 0,
        log: [],
        asking: null, // 正在等用户回答时的 { resolve, hits, options }
    };

    let deck = new Deck(null);
    let buttons = [];
    let pending = false; // 防止渲染并发
    let timer = null;

    // ---- 设备 ------------------------------------------------------------
    const pushLog = (line) => {
        state.log.push(line);
        state.log = state.log.slice(-6);
    };

    const page = () => ({
        page: 'status',
        project: state.project,
        state: state.status,
        detail: state.detail,
        target: state.target,
        progress: state.progress,
        elapsed: state.elapsed,
        log: state.log,
        buttons: state.asking ? null : undefined,
    });

    async function paint({ force = false } = {}) {
        if (!deck.connected || pending) return 0;
        pending = true;
        try {
            const job = page();
            const { rgb } = await render(job);
            return deck.sendRgb(rgb, { force });
        } catch (err) {
            // 渲染或串口出错不该影响 agent 干活 —— 这块屏是锦上添花。
            ctx.logger?.debug?.(`mosaico-deck: ${err.message}`);
            return 0;
        } finally {
            pending = false;
        }
    }

    async function showAsk(question, options) {
        const { rgb, hits } = await render({
            page: 'ask',
            question,
            options,
            footer: '点一下选择，或按 AI 键取消',
        });
        buttons = hits;
        await deck.sendRgb(rgb, { force: true });
    }

    function connect() {
        if (deck.connected) return true;
        if (!deck.open()) return false;
        state.connected = true;
        // 板子刚上电时还在跑开机动画（约 1.9s），这时候推帧会：
        //   · 和动画抢 SPI 总线
        //   · 把动画打断
        // 但**不能等太久** —— 板子的动画现在没有"终点"，它会一直画到 PC 接管，
        // 所以这里等 1.6s（动画主体差不多放完）就接手，屏幕不会出现空档。
        // 原来写的是 1200ms 固定等，加上动画自己尾巴的黑屏，用户看到的是
        // "炸开之后黑了好一会儿"。
        setTimeout(() => paint({ force: true }), 1600);
        return true;
    }

    deck.on('event', (ev) => {
        if (ev.t === 'touch' && ev.ev === 'down') {
            const hit = buttons.find(
                (b) =>
                    ev.x >= b.x - 6 && ev.x <= b.x + b.w + 6 &&
                    ev.y >= b.y - 6 && ev.y <= b.y + b.h + 6,
            );
            if (hit && state.asking) {
                const r = state.asking;
                state.asking = null;
                buttons = [];
                r.resolve(Number.isInteger(+hit.value)
                    ? r.options[+hit.value]
                    : hit.value);
            }
        } else if (ev.t === 'key' && state.asking) {
            // 物理按键 = 取消。比在屏幕上找"取消"顺手。
            const r = state.asking;
            state.asking = null;
            buttons = [];
            r.resolve('cancelled');
        }
    });

    deck.on('audio', (pcm) => {
        state._audio = Buffer.concat([state._audio || Buffer.alloc(0), pcm]);
    });

    // ---- 生命周期：界面自动跟着走 ----------------------------------------
    ctx.on('agent/status', ({ status }) => {
        if (status === 'running') {
            if (state.status === 'idle') {
                state.startedAt = Date.now();
                state.log = [];
            }
            state.status = 'thinking';
            state.detail = '正在处理';
            state.target = '';
        } else {
            state.status = 'idle';
        }
        void paint();
    });

    ctx.on('tools/pre-execute', (payload) => {
        try {
            const call = payload?.call ?? payload?.toolCall ?? payload;
            const toolName = call?.name ?? call?.tool ?? 'tool';
            state.status = 'tool';
            state.detail = `正在调用 ${toolName}`;
            const args = call?.arguments ?? call?.args;
            if (args && typeof args === 'object') {
                const first = args.command || args.path || args.file_path || args.query;
                if (first) state.target = String(first).slice(0, 34);
            }
            pushLog(`调用 ${toolName}`);
            state.progress = null;
        } catch {
            /* 事件结构变了也不该崩 */
        }
        void paint();
    });

    ctx.on('tools/post-execute', () => {
        state.status = 'thinking';
        state.detail = '工具跑完了';
        void paint();
    });

    ctx.on('assistant/message', (payload) => {
        try {
            const text = payload?.message?.content
                ?.map?.((c) => c?.text ?? '')
                .join('') ?? '';
            if (text) {
                state.status = 'running';
                state.detail = text.replace(/\s+/g, ' ').slice(0, 30);
                pushLog('回复了一段');
            }
        } catch {
            /* 同上 */
        }
        void paint();
    });

    ctx.on('agent/error', () => {
        state.status = 'error';
        state.detail = '出错了';
        pushLog('出错');
        void paint({ force: true });
    });

    // ---- 连接维护 --------------------------------------------------------
    connect();
    const reconnect = setInterval(() => {
        if (deck.connected) return;
        connect();
    }, RECONNECT_MS);

    timer = setInterval(() => {
        if (!deck.connected) return;
        const next = state.status === 'idle' ? 0
            : Math.floor((Date.now() - state.startedAt) / 1000);
        if (next !== state.elapsed) {
            state.elapsed = next;
            void paint();
        }
    }, TICK_MS);

    ctx.on('dispose', () => {
        clearInterval(reconnect);
        clearInterval(timer);
        deck.close();
    });

    // ---- 工具 ------------------------------------------------------------
    ctx.tools.register(defineTool({
        name: 'deck_speak',
        description:
            '让桌上的 mosaico-deck 设备把这句话念出来。适合「干完了」「需要你看一眼」'
            + '这类不打断但希望被知道的时刻 —— 它就在你桌上。',
        parameters: {
            text: { type: 'string', required: true, description: '要说的内容' },
            voice: { type: 'string', description: 'macOS 音色名，中文常用 Tingting' },
        },
        output: { schema: { type: 'json' }, render: (_a, v) => [{ type: 'text', text: v.message }] },
        async execute(args) {
            if (!deck.connected) return { ok: false, message: '设备没连上' };
            const pcm = await textToPcm(args.text, args.voice);
            const ms = await deck.speakPcm(pcm);
            return { ok: true, message: `说完了（${(pcm.length / 192000).toFixed(1)} 秒，用时 ${ms}ms）` };
        },
        presentCall: (a) => ({ card: 'generic', kind: 'read', title: `说话：${a.text?.slice(0, 20) ?? ''}` }),
    }));

    ctx.tools.register(defineTool({
        name: 'deck_ask',
        description:
            '在 mosaico-deck 设备上向用户提问并阻塞等待回答。**这是这个设备最有用的地方**'
            + ' —— 人不用切窗口、不用在终端里找提示，设备亮起来、他按一下就回答了。'
            + '返回选项原文；超时返回 timeout，用户按键取消返回 cancelled。',
        parameters: {
            question: { type: 'string', required: true },
            options: { type: 'array', items: { type: 'string' }, required: true, description: '2~5 个选项' },
            timeoutSec: { type: 'number', description: '默认 120' },
        },
        output: { schema: { type: 'json' }, render: (_a, v) => [{ type: 'text', text: v.answer }] },
        async execute(args, exec) {
            if (!deck.connected) return { answer: '设备没连上' };
            const options = (args.options || []).slice(0, 5);
            const timeout = (args.timeoutSec || 120) * 1000;
            const answer = await new Promise((resolve) => {
                state.asking = { resolve, options };
                void showAsk(args.question, options);
                const t = setTimeout(() => {
                    if (state.asking) {
                        state.asking = null;
                        buttons = [];
                        resolve('timeout');
                    }
                }, timeout);
                exec?.signal?.addEventListener?.('abort', () => {
                    clearTimeout(t);
                    if (state.asking) { state.asking = null; buttons = []; resolve('cancelled'); }
                });
            });
            void paint({ force: true });
            return { answer };
        },
        presentCall: (a) => ({ card: 'generic', kind: 'read', title: `问用户：${a.question?.slice(0, 20) ?? ''}` }),
    }));

    ctx.tools.register(defineTool({
        name: 'deck_show',
        description: '直接设置 mosaico-deck 上显示的状态（一般不用 —— 状态会自动跟着 agent 走）。',
        parameters: {
            detail: { type: 'string' },
            target: { type: 'string' },
            state: { type: 'string', enum: ['idle', 'thinking', 'running', 'editing', 'testing', 'waiting', 'done', 'error'] },
            progress: { type: 'number' },
            logLine: { type: 'string' },
        },
        output: { schema: { type: 'json' }, render: (_a, v) => [{ type: 'text', text: v.message }] },
        async execute(args) {
            if (args.detail !== undefined) state.detail = String(args.detail);
            if (args.target !== undefined) state.target = String(args.target);
            if (args.state !== undefined) state.status = String(args.state);
            if (args.progress !== undefined) state.progress = Number(args.progress);
            if (args.logLine) pushLog(String(args.logLine));
            const n = await paint();
            return { message: n ? `已更新（${n} 条 band）` : '没变化' };
        },
        presentCall: () => ({ card: 'generic', kind: 'read', title: '更新设备状态' }),
    }));

    ctx.logger?.info?.(`mosaico-deck: ${checkEnvironment().python} / 端口 ${findPort() || '未找到'}`);
}

/** 用 macOS 的 say + ffmpeg 生成板子要的 48kHz 立体声 PCM。
 *
 * TTS 放在 PC 上：板子只有 512KB SRAM、没有神经网络加速，跑不了任何像样的
 * TTS；而系统自带的 `say` 音质好、支持中文。板子只负责把 PCM 播出来。
 */
async function textToPcm(text, voice) {
    const { execFile } = await import('node:child_process');
    const os = await import('node:os');
    const path = await import('node:path');
    const fs = await import('node:fs');
    const { promisify } = await import('node:util');
    const run = promisify(execFile);

    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-'));
    const aiff = path.join(dir, 't.aiff');
    const pcm = path.join(dir, 't.pcm');
    try {
        const sayArgs = ['-o', aiff];
        if (voice) sayArgs.push('-v', voice);
        sayArgs.push(text);
        await run('say', sayArgs, { timeout: 60000 });
        await run('ffmpeg', ['-y', '-loglevel', 'error', '-i', aiff,
            '-f', 's16le', '-acodec', 'pcm_s16le', '-ar', '48000', '-ac', '2', pcm],
            { timeout: 60000 });
        return fs.readFileSync(pcm);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}
