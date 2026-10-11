// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// selftest.mjs —— 重启前把插件安全地"预演"一遍
//
// 为什么需要它：profile 的 bundle 只在启动时组装，改完插件必须重启 DSH 才生效。
// 而**如果插件在加载时抛异常，可能连 DSH 都起不来** —— 那时候连改回来的机会
// 都没有。所以重启前先用一个模拟的 cordis ctx 把 apply() 真跑一遍。
//
//   node plugin/selftest.mjs
//
// 它检查：模块能 import、导出了 name/inject/apply、apply() 不抛异常、
// 工具注册成功、事件处理器在收到空/null payload 时也不崩。

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

const spec = process.argv[2] || 'dsh-mosaico-deck';
let mod;
try {
    mod = await import(spec);
} catch (e) {
    console.log(`  ❌ import 失败: ${e.message.split('\n')[0]}`);
    process.exit(1);
}
console.log('  ✅ import 成功');

if (typeof mod.apply !== 'function' || typeof mod.name !== 'string') {
    console.log('  ❌ 必须导出 name 和 apply');
    process.exit(1);
}
console.log(`  ✅ name = ${mod.name}  inject = ${JSON.stringify(mod.inject)}`);

try {
    mod.apply(ctx);
} catch (e) {
    console.log(`  ❌ apply() 抛异常: ${e.message}`);
    console.log(e.stack.split('\n').slice(0, 4).join('\n'));
    process.exit(1);
}
console.log('  ✅ apply() 正常返回');
console.log(`  ✅ 注册了 ${registered.length} 个工具: ${registered.map((t) => t.name).join(', ')}`);
console.log(`  ✅ 监听了 ${listeners.size} 个事件: ${[...listeners.keys()].join(', ')}`);

const fire = (ev, payload) => {
    let ok = true;
    for (const fn of listeners.get(ev) || []) {
        try { fn(payload); } catch (e) {
            console.log(`  ❌ 事件 ${ev} 的处理器抛异常: ${e.message}`);
            ok = false;
        }
    }
    return ok;
};

// **空 payload 和 null 都要试** —— 宿主事件的结构不保证，插件不能挑食。
let ok = true;
ok = fire('agent/status', { status: 'running' }) && ok;
ok = fire('agent/status', { status: 'idle' }) && ok;
ok = fire('tools/pre-execute', { call: { name: 'bash', arguments: { command: 'ls' } } }) && ok;
ok = fire('tools/pre-execute', {}) && ok;
ok = fire('tools/pre-execute', null) && ok;
ok = fire('tools/post-execute', {}) && ok;
ok = fire('assistant/message', { message: { content: [{ type: 'text', text: '你好' }] } }) && ok;
ok = fire('assistant/message', {}) && ok;
ok = fire('agent/error', {}) && ok;
console.log(ok ? '  ✅ 所有事件处理器都没崩（含空/null payload）' : '  ❌ 有处理器抛异常');

for (const fn of listeners.get('dispose') || []) fn();
console.log(`  ✅ dispose 正常  ${logs[0] ? `| ${logs[0].slice(0, 90)}` : ''}`);
console.log('\n✅ 可以安全重启 DeepSeek Harness');
