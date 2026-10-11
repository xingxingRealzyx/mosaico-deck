// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// audio.cpp —— ES8311 音频

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

// 官方 esp_mosaico.h 里的引脚。写死在这里而不是从 board.h 取 —— 因为它们是
// 音频专属的，不属于"随板子版本变化"的那组（V1.0 和 V1.2 的音频引脚相同）。
constexpr gpio_num_t kMclk = GPIO_NUM_54;
constexpr gpio_num_t kBclk = GPIO_NUM_37;
constexpr gpio_num_t kWs = GPIO_NUM_49;
constexpr gpio_num_t kDout = GPIO_NUM_52;  // 放音
constexpr gpio_num_t kDin = GPIO_NUM_40;   // 录音
constexpr gpio_num_t kPaCtrl = GPIO_NUM_45;

// codec 的 I2C 地址。7 位是 0x19，而 esp_codec_dev 要的是**8 位形式**：
// 官方注释专门提了这一点（`BSP_AUDIO_CODEC_I2C_ADDR (0x19U << 1)`）。
// 传 7 位的话 codec 一辈子配不上，而且错误信息只会说"create codec failed"。
constexpr uint8_t kCodecAddr = 0x19U << 1;

i2s_chan_handle_t g_tx = nullptr;
i2s_chan_handle_t g_rx = nullptr;
const audio_codec_data_if_t* g_data_if = nullptr;
esp_codec_dev_handle_t g_spk = nullptr;
esp_codec_dev_handle_t g_mic = nullptr;

// esp_codec_dev 不是线程安全的（同一时刻只能有一个 read 或 write 在跑）。
// 放音任务和录音任务会并发，所以必须串行化。
osal::Mutex g_io_lock;

esp_err_t InitI2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // auto_clear：TX 没数据时自动补静音。不加的话喇叭会一直重复最后一帧，
    // 表现是放完之后有个"嗡嗡"的拖音。
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &g_tx, &g_rx), kTag,
                        "创建 I2S 通道失败");

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
    // 256 倍 MCLK —— 官方也是这个值。ES8311 靠它分频出内部时钟。
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_tx, &std_cfg), kTag, "初始化 I2S TX 失败");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_rx, &std_cfg), kTag, "初始化 I2S RX 失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_tx), kTag, "使能 I2S TX 失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_rx), kTag, "使能 I2S RX 失败");

    // 注意：这个接口要的是**非 const** 指针（它会改写 clk_src 字段）。
    audio_codec_i2s_cfg_t data_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = g_rx,
        .tx_handle = g_tx,
    };
    g_data_if = audio_codec_new_i2s_data(&data_cfg);
    ESP_RETURN_ON_FALSE(g_data_if != nullptr, ESP_FAIL, kTag, "创建 I2S data 接口失败");
    return ESP_OK;
}

esp_codec_dev_handle_t CreateCodec(esp_codec_dev_type_t type, void* i2c_bus, int16_t pa_pin) {
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
    if (gpio_if == nullptr) {
        ESP_LOGE(kTag, "创建 GPIO 接口失败");
        return nullptr;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.addr = kCodecAddr;
    i2c_cfg.bus_handle = i2c_bus;
    i2c_cfg.clock_speed_hz = 0;  // 0 = 用总线默认
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == nullptr) {
        ESP_LOGE(kTag, "创建 ES8311 I2C 控制接口失败（地址 0x%02X）", kCodecAddr);
        return nullptr;
    }

    es8311_codec_cfg_t codec_cfg = {};
    codec_cfg.ctrl_if = ctrl_if;
    codec_cfg.gpio_if = gpio_if;
    codec_cfg.codec_mode = (type == ESP_CODEC_DEV_TYPE_OUT) ? ESP_CODEC_DEV_WORK_MODE_DAC
                                                            : ESP_CODEC_DEV_WORK_MODE_ADC;
    // 功放使能脚由 codec 驱动代管 —— 放音时它自己拉高、停播时拉低。
    codec_cfg.pa_pin = pa_pin;
    codec_cfg.master_mode = false;  // ESP 是 I2S 主机
    codec_cfg.use_mclk = true;
    codec_cfg.digital_mic = false;
    codec_cfg.hw_gain.pa_voltage = 5.0f;
    codec_cfg.hw_gain.codec_dac_voltage = 3.3f;
    // no_dac_ref：DAC 不输出参考电压。官方也开着 —— 这个板子的功放不需要它。
    codec_cfg.no_dac_ref = true;

    const audio_codec_if_t* codec_if = es8311_codec_new(&codec_cfg);
    if (codec_if == nullptr) {
        ESP_LOGE(kTag, "创建 ES8311 codec 失败（%s）",
                 (type == ESP_CODEC_DEV_TYPE_OUT) ? "放音" : "录音");
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

    ESP_RETURN_ON_ERROR(InitI2s(), kTag, "I2S 初始化失败");
    // codec 上电后要等一小会儿才能配寄存器（官方等了 10ms）。
    osal::Delay(10);

    g_spk = CreateCodec(ESP_CODEC_DEV_TYPE_OUT, i2c_bus, kPaCtrl);
    g_mic = CreateCodec(ESP_CODEC_DEV_TYPE_IN, i2c_bus, GPIO_NUM_NC);

    if (g_spk == nullptr && g_mic == nullptr) {
        ESP_LOGE(kTag, "喇叭和麦克风都没建起来");
        return ESP_FAIL;
    }

    // 打开 codec。**必须先 open 再设音量/增益** —— 没 open 时那些写寄存器的
    // 调用会失败，而且音量会停在 codec 的上电默认值（常常是静音）。
    if (g_spk != nullptr) {
        esp_codec_dev_sample_info_t fs = {};
        fs.sample_rate = kSampleRate;
        fs.channel = kChannels;
        fs.bits_per_sample = kBitsPerSample;
        if (esp_codec_dev_open(g_spk, &fs) != ESP_OK) {
            ESP_LOGW(kTag, "打开喇叭失败");
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
            ESP_LOGW(kTag, "打开麦克风失败");
        } else {
            esp_codec_dev_set_in_gain(g_mic, 36.0);
        }
    }

    ESP_LOGI(kTag, "ES8311 就绪：%d Hz / %d bit / %d 声道  喇叭=%s 麦克风=%s",
             kSampleRate, kBitsPerSample, kChannels, g_spk ? "有" : "无",
             g_mic ? "有" : "无");
    return ESP_OK;
}

bool ready() { return g_spk != nullptr || g_mic != nullptr; }

size_t Play(const void* buf, size_t size, uint32_t timeout_ms) {
    if (g_spk == nullptr || buf == nullptr || size == 0) {
        return 0;
    }
    osal::LockGuard<osal::Mutex> guard(g_io_lock);

    // 超时的换算：esp_codec_dev 的超时单位是 ms，而这里给的是"写完全部数据"的
    // 上限。用固定值就够 —— 192KB/s 下 1 秒能写 192KB，远大于单次 chunk。
    (void)timeout_ms;
    const int written = esp_codec_dev_write(g_spk, const_cast<void*>(buf), static_cast<int>(size));
    return (written > 0) ? static_cast<size_t>(written) : 0;
}

esp_err_t SetVolume(int percent) {
    if (g_spk == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // esp_codec_dev 的音量范围是 0~100（不是 dB）。
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
    // 输入增益是按 dB 算的（esp_codec_dev 的接口如此），这里做个体面的映射：
    // 0~100 线性映射到 0~42dB。36dB 大约是官方给的默认值。
    const float db = static_cast<float>(percent) * 42.0f / 100.0f;
    return esp_codec_dev_set_in_gain(g_mic, db);
}

}  // namespace audio
