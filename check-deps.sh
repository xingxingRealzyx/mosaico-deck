#!/usr/bin/env bash
#
# check-deps.sh -- 检查 PC 端依赖是否齐全。
#
# 这块板子本身只要有固件就能显示，但**插件**是在 PC 上渲染和发声的，所以它需要
# 一些外部程序。缺了它们不会崩，而是"面板空白"或"喇叭不响"这类难查的现象 ——
# 这个脚本把答案直接摆出来。
#
# 用法: ./check-deps.sh
set -u

OK="  ✓"; BAD="  ✗"; WARN="  !"
missing=0
optional_missing=0

have() { command -v "$1" >/dev/null 2>&1; }

row() { # name why status ok?
    if [ "$4" = "1" ]; then printf "%s %-14s %s\n" "$OK" "$1" "$2"
    else printf "%s %-14s %s\n" "$3" "$1" "$2"; fi
}

echo "PC 端依赖"
echo

# ---- 必需：没有它面板根本画不出来 ----
if have python3; then ok=1; else ok=0; missing=$((missing+1)); fi
row "python3" "渲染界面（plugin/render.py）" "$BAD" "$ok"
if [ "$ok" = "1" ]; then
    if python3 -c "import PIL" 2>/dev/null; then
        v=$(python3 -c "import PIL; print(PIL.__version__)" 2>/dev/null)
        printf "%s %-14s Pillow %s\n" "$OK" "  └ Pillow" "$v"
    else
        printf "%s %-14s 没装：python3 -m pip install pillow\n" "$BAD" "  └ Pillow"
        missing=$((missing+1))
    fi
fi

# ---- 语音输出：非必需，缺了就只是不发声 ----
if have say; then ok=1; else ok=0; optional_missing=$((optional_missing+1)); fi
row "say" "朗读播报（macOS 自带；Linux 需另配）" "$WARN" "$ok"

if have ffmpeg; then ok=1; else ok=0; optional_missing=$((optional_missing+1)); fi
row "ffmpeg" "语音编解码（朗读与语音输入都要）" "$WARN" "$ok"

# ---- 语音输入：非必需，缺了就只是不能按住说话 ----
WHISPER="${DECK_WHISPER:-whisper-cli}"
if have "$WHISPER"; then ok=1; else ok=0; optional_missing=$((optional_missing+1)); fi
row "$WHISPER" "语音转文字（按键说话）" "$WARN" "$ok"

MODEL="${DECK_WHISPER_MODEL:-$HOME/.cache/whisper/ggml-small.bin}"
if [ -f "$MODEL" ]; then ok=1; else ok=0; optional_missing=$((optional_missing+1)); fi
row "whisper 模型" "$MODEL" "$WARN" "$ok"

echo
echo "固件开发（只在需要编译/烧录时用）"
for c in idf.py esptool; do
    if have "$c"; then printf "%s %-14s\n" "$OK" "$c"
    else printf "%s %-14s 见 README 的固件一节\n" "$WARN" "$c"; fi
done

echo
if [ "$missing" -gt 0 ]; then
    echo "缺少 $missing 个必需依赖 —— 面板无法工作。运行 ./install-deps.sh 安装。"
    exit 1
fi
if [ "$optional_missing" -gt 0 ]; then
    echo "必需依赖齐全。有 $optional_missing 个可选组件没装（语音相关），"
    echo "面板与按键说话之外的功能不受影响。装法见 ./install-deps.sh"
    exit 0
fi
echo "全部齐全。"
