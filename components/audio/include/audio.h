// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// audio.h —— ES8311 音频（喇叭 + 麦克风）
//
// 这块板子本质上是个完整的语音设备：ES8311 codec + 功放 + 麦克风 + I2S。
// 官方 BSP 的 `BSP_CAPS_AUDIO_SPEAKER` 和 `BSP_CAPS_AUDIO_MIC` 都是 1。
//
// 引脚（官方 esp_mosaico.h）
// ------------------------
//   MCLK=GPIO54  BCLK=GPIO37  WS=GPIO49  DOUT=GPIO52（放）  DIN=GPIO40（录）
//   PA_CTRL=GPIO45（功放使能，由 codec 驱动代管）
//   codec 地址 0x19（8 位形式 0x32）
//
// **codec 供电在 V1.2 上不用管。** 官方 bsp_power_set_codec_3v3() 里有一句
// `if (!is_v1_0()) return ESP_OK;` —— V1.2 取消了那个控制脚（GPIO56 被挪去当
// I2C SDA 了）。本工程实测就是 V1.2，所以不碰它。

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace audio {

// 和官方 BSP 一致：48kHz / 16bit / 立体声。
//
// 为什么不做 16kHz 单声道省带宽：codec 的时钟是初始化时按采样率配好的，
// 换采样率要连同 MCLK 分频一起改，而 USB 的 1.5MB/s 完全带得动 192KB/s。
// 语音场景 48kHz 是浪费，但省下来的是复杂度，不是瓶颈。
inline constexpr int kSampleRate = 48000;
inline constexpr int kChannels = 2;
inline constexpr int kBitsPerSample = 16;
inline constexpr size_t kFrameBytes = kChannels * (kBitsPerSample / 8);  // 4

// I2S + codec（DAC 和 ADC 一起建）。
// 必须在 I2C 总线建好之后调用 —— codec 是挂在 I2C 上配置的。
esp_err_t Init(void* i2c_bus);

bool ready();

// ---- 放音 ------------------------------------------------------------------
// buf 是**交错立体声** 16 位 PCM。size 是字节数。
// 阻塞写，超时返回实际写入的字节数（可能少于 size）。
size_t Play(const void* buf, size_t size, uint32_t timeout_ms);

// 音量 0~100。
esp_err_t SetVolume(int percent);

// ---- 录音 ------------------------------------------------------------------
// 读出交错立体声 16 位 PCM。返回实际读到的字节数。
size_t Record(void* buf, size_t size, uint32_t timeout_ms);

// 麦克风的音量（0~100）。
esp_err_t SetMicGain(int percent);

}  // namespace audio
