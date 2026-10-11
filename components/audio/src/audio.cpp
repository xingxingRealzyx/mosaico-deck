// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// audio.cpp -- ES8311 audio

#include "audio.h"

#include <cstring>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "fbox/board.h"
#include "osal.h"

namespace audio {
namespace {

constexpr const char* kTag = "audio";

// Pins from the official esp_mosaico.h. Hardcoded here rather than taken from board.h --
// because they are audio-specific and do not belong to the group that "varies with the
// board revision" (V1.0 and V1.2 have the same audio pins).
constexpr gpio_num_t kMclk = GPIO_NUM_54;
constexpr gpio_num_t kBclk = GPIO_NUM_37;
constexpr gpio_num_t kWs = GPIO_NUM_49;
constexpr gpio_num_t kDout = GPIO_NUM_52;  // playback
constexpr gpio_num_t kDin = GPIO_NUM_40;   // recording
constexpr gpio_num_t kPaCtrl = GPIO_NUM_45;

// The codec's I2C address. It is 0x19 in 7-bit form, but esp_codec_dev wants the
// **8-bit form**: the official comment calls this out specifically
// (`BSP_AUDIO_CODEC_I2C_ADDR (0x19U << 1)`).
// If you pass the 7-bit form, the codec can never be configured, and all the error
// message says is "create codec failed".
constexpr uint8_t kCodecAddr = 0x19U << 1;

i2s_chan_handle_t g_tx = nullptr;
i2s_chan_handle_t g_rx = nullptr;
const audio_codec_data_if_t* g_data_if = nullptr;
esp_codec_dev_handle_t g_spk = nullptr;
esp_codec_dev_handle_t g_mic = nullptr;

// esp_codec_dev is not thread-safe (only one read or write can be running at a time).
// The playback task and the recording task run concurrently, so they must be serialized.
osal::Mutex g_io_lock;

esp_err_t InitI2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // auto_clear: automatically fill in silence when TX has no data. Without it the
    // speaker would keep repeating the last frame, which shows up as a "humming" trailing
    // tone after playback finishes.
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &g_tx, &g_rx), kTag,
                        "failed to create I2S channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = kMclk,
            .bclk = kBclk,
            .ws = kWs,
            .dout = kDout,
            .din = kDin,
            .invert_flags = {},
        },
    };
    // 256x MCLK -- the official value too. The ES8311 divides it down to its internal
    // clock.
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_tx, &std_cfg), kTag, "failed to init I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_rx, &std_cfg), kTag, "failed to init I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_tx), kTag, "failed to enable I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_rx), kTag, "failed to enable I2S RX");

    // Note: this interface wants a **non-const** pointer (it rewrites the clk_src field).
    audio_codec_i2s_cfg_t data_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = g_rx,
        .tx_handle = g_tx,
    };
    g_data_if = audio_codec_new_i2s_data(&data_cfg);
    ESP_RETURN_ON_FALSE(g_data_if != nullptr, ESP_FAIL, kTag, "failed to create I2S data interface");
    return ESP_OK;
}

esp_codec_dev_handle_t CreateCodec(esp_codec_dev_type_t type, void* i2c_bus, int16_t pa_pin) {
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
    if (gpio_if == nullptr) {
        ESP_LOGE(kTag, "failed to create GPIO interface");
        return nullptr;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.addr = kCodecAddr;
    i2c_cfg.bus_handle = i2c_bus;
    i2c_cfg.clock_speed_hz = 0;  // 0 = use the bus default
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == nullptr) {
        ESP_LOGE(kTag, "failed to create ES8311 I2C control interface (addr 0x%02X)", kCodecAddr);
        return nullptr;
    }

    es8311_codec_cfg_t codec_cfg = {};
    codec_cfg.ctrl_if = ctrl_if;
    codec_cfg.gpio_if = gpio_if;
    codec_cfg.codec_mode = (type == ESP_CODEC_DEV_TYPE_OUT) ? ESP_CODEC_DEV_WORK_MODE_DAC
                                                            : ESP_CODEC_DEV_WORK_MODE_ADC;
    // The power amplifier enable pin is managed by the codec driver -- it pulls it high
    // during playback and low when playback stops.
    codec_cfg.pa_pin = pa_pin;
    codec_cfg.master_mode = false;  // the ESP is the I2S master
    codec_cfg.use_mclk = true;
    codec_cfg.digital_mic = false;
    codec_cfg.hw_gain.pa_voltage = 5.0f;
    codec_cfg.hw_gain.codec_dac_voltage = 3.3f;
    // no_dac_ref: the DAC does not output a reference voltage. The official code enables
    // it too -- this board's power amplifier does not need it.
    codec_cfg.no_dac_ref = true;

    const audio_codec_if_t* codec_if = es8311_codec_new(&codec_cfg);
    if (codec_if == nullptr) {
        ESP_LOGE(kTag, "failed to create ES8311 codec (%s)",
                 (type == ESP_CODEC_DEV_TYPE_OUT) ? "playback" : "recording");
        return nullptr;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = type;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if = g_data_if;
    return esp_codec_dev_new(&dev_cfg);
}

}  // namespace

esp_err_t Init(void* i2c_bus) {
    if (g_spk != nullptr || g_mic != nullptr) {
        return ESP_OK;
    }
    if (i2c_bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(InitI2s(), kTag, "I2S init failed");
    // After the codec is powered on it needs a short while before its registers can be
    // configured (the official code waits 10ms).
    osal::Delay(10);

    g_spk = CreateCodec(ESP_CODEC_DEV_TYPE_OUT, i2c_bus, kPaCtrl);
    g_mic = CreateCodec(ESP_CODEC_DEV_TYPE_IN, i2c_bus, GPIO_NUM_NC);

    if (g_spk == nullptr && g_mic == nullptr) {
        ESP_LOGE(kTag, "neither speaker nor microphone could be created");
        return ESP_FAIL;
    }

    // Open the codec. **It must be opened before setting volume/gain** -- those
    // register-writing calls fail when it is not open, and the volume stays at the
    // codec's power-on default (often mute).
    if (g_spk != nullptr) {
        esp_codec_dev_sample_info_t fs = {};
        fs.sample_rate = kSampleRate;
        fs.channel = kChannels;
        fs.bits_per_sample = kBitsPerSample;
        if (esp_codec_dev_open(g_spk, &fs) != ESP_OK) {
            ESP_LOGW(kTag, "failed to open speaker");
        } else {
            esp_codec_dev_set_out_vol(g_spk, 75);
        }
    }
    if (g_mic != nullptr) {
        esp_codec_dev_sample_info_t fs = {};
        fs.sample_rate = kSampleRate;
        fs.channel = kChannels;
        fs.bits_per_sample = kBitsPerSample;
        if (esp_codec_dev_open(g_mic, &fs) != ESP_OK) {
            ESP_LOGW(kTag, "failed to open microphone");
        } else {
            esp_codec_dev_set_in_gain(g_mic, 36.0);
        }
    }

    ESP_LOGI(kTag, "ES8311 ready: %d Hz / %d bit / %d channels  speaker=%s mic=%s",
             kSampleRate, kBitsPerSample, kChannels, g_spk ? "yes" : "no",
             g_mic ? "yes" : "no");
    return ESP_OK;
}

bool ready() { return g_spk != nullptr || g_mic != nullptr; }

size_t Play(const void* buf, size_t size, uint32_t timeout_ms) {
    if (g_spk == nullptr || buf == nullptr || size == 0) {
        return 0;
    }
    osal::LockGuard<osal::Mutex> guard(g_io_lock);

    // Timeout conversion: esp_codec_dev's timeout unit is ms, while what is given here is
    // the upper bound for "writing all the data". A fixed value is enough -- at 192KB/s,
    // 1 second writes 192KB, far more than a single chunk.
    (void)timeout_ms;
    const int written = esp_codec_dev_write(g_spk, const_cast<void*>(buf), static_cast<int>(size));
    return (written > 0) ? static_cast<size_t>(written) : 0;
}

esp_err_t SetVolume(int percent) {
    if (g_spk == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // esp_codec_dev's volume range is 0~100 (not dB).
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    return esp_codec_dev_set_out_vol(g_spk, percent);
}

size_t Record(void* buf, size_t size, uint32_t timeout_ms) {
    if (g_mic == nullptr || buf == nullptr || size == 0) {
        return 0;
    }
    osal::LockGuard<osal::Mutex> guard(g_io_lock);
    (void)timeout_ms;
    const int got = esp_codec_dev_read(g_mic, buf, static_cast<int>(size));
    return (got > 0) ? static_cast<size_t>(got) : 0;
}

esp_err_t SetMicGain(int percent) {
    if (g_mic == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // The input gain is measured in dB (that is what esp_codec_dev's interface does), so
    // here is a decent mapping: 0~100 maps linearly to 0~42dB. 36dB is roughly the
    // official default.
    const float db = static_cast<float>(percent) * 42.0f / 100.0f;
    return esp_codec_dev_set_in_gain(g_mic, db);
}

}  // namespace audio
