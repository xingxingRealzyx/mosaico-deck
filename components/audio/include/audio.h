// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// audio.h -- ES8311 audio (speaker + microphone)
//
// This board is essentially a complete voice device: ES8311 codec + power amplifier +
// microphone + I2S. The official BSP's `BSP_CAPS_AUDIO_SPEAKER` and
// `BSP_CAPS_AUDIO_MIC` are both 1.
//
// Pins (official esp_mosaico.h)
// ----------------------------
//   MCLK=GPIO54  BCLK=GPIO37  WS=GPIO49  DOUT=GPIO52 (out)  DIN=GPIO40 (in)
//   PA_CTRL=GPIO45 (power amplifier enable, managed by the codec driver)
//   codec address 0x19 (8-bit form 0x32)
//
// **codec power does not need to be handled on V1.2.** The official
// bsp_power_set_codec_3v3() contains
// `if (!is_v1_0()) return ESP_OK;` -- V1.2 removed that control pin (GPIO56 was moved
// to be I2C SDA). This project has been measured to be V1.2, so we leave it alone.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace audio {

// Same as the official BSP: 48kHz / 16bit / stereo.
//
// Why not use 16kHz mono to save bandwidth: the codec clock is configured for the sample
// rate at init time, so changing the sample rate means also changing the MCLK divider,
// and USB's 1.5MB/s carries 192KB/s without any trouble.
// 48kHz is wasteful for voice, but what it saves is complexity, not a bottleneck.
inline constexpr int kSampleRate = 48000;
inline constexpr int kChannels = 2;
inline constexpr int kBitsPerSample = 16;
inline constexpr size_t kFrameBytes = kChannels * (kBitsPerSample / 8);  // 4

// I2S + codec (DAC and ADC are created together).
// Must be called after the I2C bus has been created -- the codec is configured over I2C.
esp_err_t Init(void* i2c_bus);

bool ready();

// -- playback --------------------------------
// buf is **interleaved stereo** 16-bit PCM. size is a byte count.
// Blocking write; on timeout it returns the number of bytes actually written (may be
// less than size).
size_t Play(const void* buf, size_t size, uint32_t timeout_ms);

// Volume 0~100.
esp_err_t SetVolume(int percent);

// -- recording -------------------------------
// Reads out interleaved stereo 16-bit PCM. Returns the number of bytes actually read.
size_t Record(void* buf, size_t size, uint32_t timeout_ms);

// Microphone volume (0~100).
esp_err_t SetMicGain(int percent);

}  // namespace audio
