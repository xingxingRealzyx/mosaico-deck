#!/usr/bin/env bash
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# install-plugin.sh —— 把插件装/更新到 DeepSeek Harness 的 desktop profile
#
# 为什么要复制而不是 link
# --------------------
# 插件里 `import { defineTool } from '@deepseek-ai/dsh-tools'`，而 Node 是
# **从插件所在目录往上找** node_modules 的。DSH 自己的包都在
# `~/.dsh/profiles/node_modules/` 下 —— 所以插件必须真的落在
# `~/.dsh/profiles/<profile>/node_modules/` 里，符号链接（pnpm 的 `link:`）
# 会解析到源目录，那条路径上根本没有 @deepseek-ai/*。
#
# 所以用 `file:` 协议（真实复制）。改完源码要重新跑这个脚本。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PROFILE="${1:-desktop}"
DSH="/Applications/DeepSeek Harness.app/Contents/Resources/runtime/cli/bin/dsh"

export DSH_HOME="${DSH_HOME:-$HOME/.dsh}"

echo "=== 安装 mosaico-deck 插件到 profile: $PROFILE ==="

# 记下 node_modules 里的实体位置（file: 装法下是个真实目录）
DEST="$DSH_HOME/profiles/$PROFILE/node_modules/dsh-mosaico-deck"

if [ -d "$DEST" ]; then
    echo "  更新已有的安装：$DEST"
    rm -rf "$DEST"
    mkdir -p "$DEST"
    cp -R "$HERE/plugin/lib" "$DEST/lib"
    cp "$HERE/plugin/render.py" "$DEST/render.py"
    cp "$HERE/plugin/package.json" "$DEST/package.json"
    cp "$HERE/plugin/cordis.patch.yml" "$DEST/cordis.patch.yml"
    cp "$HERE/plugin/selftest.mjs" "$DEST/selftest.mjs"
else
    echo "  首次安装（需要 profile 里声明 bundle）"
    "$DSH" plugin --profile "$PROFILE" add "file:$HERE/plugin"
    python3 - "$DSH_HOME/profiles/$PROFILE/package.json" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
b = d.setdefault('dsh', {}).setdefault('profile', {}).setdefault('bundles', [])
if 'dsh-mosaico-deck' not in b:
    b.append('dsh-mosaico-deck')
    json.dump(d, open(p, 'w'), indent=2)
    print('  已把 dsh-mosaico-deck 加进 profile 的 bundles')
else:
    print('  bundles 里已经有了')
PY
fi

echo
# 自检必须在**安装后的副本**上跑 —— ESM 的裸模块名（@deepseek-ai/*）是按
# 导入方所在路径解析的，从仓库里那份跑会找不到。
NODE="/Applications/DeepSeek Harness.app/Contents/Resources/runtime/primary-runtime/dependencies/node/bin/node"
if [ -x "$NODE" ] && [ -f "$DEST/selftest.mjs" ]; then
    echo
    echo "=== 加载自检（重启前预演，防止插件异常导致 DSH 起不来）==="
    ( cd "$DSH_HOME/profiles/$PROFILE" && "$NODE" "$DEST/selftest.mjs" ) || {
        echo "❌ 自检没过，**先别重启**"
        exit 1
    }
fi

echo
echo "✅ 装好了。**需要重启 DeepSeek Harness** 才会加载（profile 的 bundle 只在启动时组装）。"
echo "   设备连上后，屏幕上会自动出现状态 —— 不需要模型调任何工具。"
