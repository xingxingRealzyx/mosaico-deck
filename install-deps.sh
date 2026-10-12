#!/usr/bin/env bash
#
# install-deps.sh -- 安装 PC 端依赖。
#
# **只在 macOS 上用 brew/pip 真正安装**；其他平台只打印该装什么，不做猜测性的
# 系统改动。语音输出目前依赖 macOS 的 say，所以语音播报这一项目前是 macOS 专属；
# 渲染、显示、语音输入本身是跨平台的。
set -u

have() { command -v "$1" >/dev/null 2>&1; }
os="$(uname -s)"

echo "平台: $os"
echo

if [ "$os" != "Darwin" ]; then
    cat <<'TXT'
这个脚本目前只在 macOS 上自动安装。在你的平台上需要：

  Python 3 + Pillow      pip install pillow
  ffmpeg                 用你的包管理器安装
  whisper.cpp            https://github.com/ggml-org/whisper.cpp  (需要 whisper-cli)
  whisper 模型           https://huggingface.co/ggerganov/whisper.cpp
                         放到 ~/.cache/whisper/ggml-small.bin（或用 DECK_WHISPER_MODEL 指定）

  语音播报还依赖 macOS 的 say 命令；其他平台暂时没有等价实现。
TXT
    exit 1
fi

if ! have brew; then
    echo "找不到 brew。先装 Homebrew: https://brew.sh" >&2
    exit 1
fi

step() { printf "\n== %s ==\n" "$1"; }

step "Pillow（渲染）"
if python3 -c "import PIL" 2>/dev/null; then
    echo "  已装"
else
    python3 -m pip install --quiet pillow || pip3 install --quiet pillow
fi

step "ffmpeg（语音编解码）"
have ffmpeg && echo "  已装" || brew install ffmpeg

step "whisper.cpp（按键说话，可选）"
have whisper-cli && echo "  已装" || brew install whisper-cpp

step "whisper 模型（可选，约 465MB）"
MODEL="${DECK_WHISPER_MODEL:-$HOME/.cache/whisper/ggml-small.bin}"
if [ -f "$MODEL" ]; then
    echo "  已有: $MODEL"
else
    mkdir -p "$(dirname "$MODEL")"
    echo "  下载到 $MODEL"
    curl -L --progress-bar -o "$MODEL" \
        https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin
fi

echo
echo "完成。用 ./check-deps.sh 复查。"
