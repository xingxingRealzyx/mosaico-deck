// Copyright 2026 ESP-Claw Authors
// SPDX-License-Identifier: MIT
//
// usb_console 实现。见 include/usb_console.h 里的原理说明。

#include "usb_console.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "vfs_tinyusb.h"

namespace usb_console {
namespace {

constexpr const char* kTag = "usb_console";
constexpr tinyusb_cdcacm_itf_t kCdcItf = TINYUSB_CDC_ACM_0;

// ---------------------------------------------------------------------------
// 直接抄自 ROM 下载模式设备的 USB 描述符（用 ioreg 实测得到）
//
//   idVendor 0x303A  idProduct 0x0020  bcdUSB 0x0200  bcdDevice 0x0764
//   bDeviceClass 0xEF / SubClass 0x02 / Protocol 0x01  bMaxPacketSize0 64
//   字符串 1="Espressif" 2="ESP32-S31" 3="1c:29:04:d0:97:d8"（MAC）
//
// 产品名里的连字符是实测出来的：macOS 系统日志里 ROM 设备叫 "ESP32-S31"，
// 而应用固件一开始写成了 "ESP32_S31"（下划线），日志里一眼能看出是两个不同的
// 设备 —— 那就破坏了"复位前后主机看到同一个设备"这个前提。
//
// PID 就是芯片 ID，所以主机在应用态和 ROM 态看到的是同一个设备。
// ---------------------------------------------------------------------------
constexpr uint16_t kEspressifVid = 0x303A;
constexpr uint16_t kChipIdPid = 0x0020;

const tusb_desc_device_t kDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,        // 0xEF
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,  // 0x02
    .bDeviceProtocol = MISC_PROTOCOL_IAD,     // 0x01
    .bMaxPacketSize0 = 64,
    .idVendor = kEspressifVid,
    .idProduct = kChipIdPid,
    .bcdDevice = 0x0764,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// "1c:29:04:d0:97:d8" —— 运行时按芯片 MAC 生成，与 ROM 完全一致。
char g_serial[18];

// tinyusb_desc_config_t::string 的类型是 const char**，所以这里不能写成
// const char* const[]（那会是指向常量的常量指针数组，转换不过去）。
const char* kStringDescriptors[] = {
    (const char[]){0x09, 0x04},  // 0: 语言 ID（英文）
    "Espressif",                 // 1: 厂商
    "ESP32-S31",                 // 2: 产品（注意连字符，不是下划线）
    g_serial,                    // 3: 序列号 = MAC
    "FluidBox CDC",              // 4: CDC 接口名
};
constexpr int kStringDescriptorCount =
    sizeof(kStringDescriptors) / sizeof(kStringDescriptors[0]);

// ---------------------------------------------------------------------------
// 免按键烧录状态
// ---------------------------------------------------------------------------
volatile bool g_reboot_to_bootloader = false;
volatile bool g_reboot_pending = false;

// 重启进 ROM 下载模式之前的回调（main 里注册，用来先关掉屏幕省电）。
PreRestartHook g_pre_restart_hook = nullptr;

// 收到的字节先进这里，由调用方取出解析。
//
// **64KB** —— 必须放得下一条 band 的 payload（480×30×2 = 28800 字节），
// 另外放音时 PC 会按播放速率推 PCM，超前量要吃得下（实测 4 秒音频约超前 16KB）。
// 一开始给的是 4KB，结果传一条 band 中途必然溢出，而当时"满了就 break"的写法
// 会让回调停止消费 USB FIFO → FIFO 满 → CDC 端点 NAK → **PC 侧 write 永久阻塞**。
// 表现就是"PC 一发 28KB，两边一起卡死"。
osal::RingBuffer* g_rx = nullptr;
volatile uint32_t g_rx_overflow = 0;

// 是否已经见过 esptool 签名里的"武装"那一步。
bool g_saw_esptool_arm = false;

esp_timer_handle_t g_restart_timer = nullptr;

void RestartTimerCallback(void* arg) {
    (void)arg;
    // 先把耗电的东西关掉再重启，避免烧录期间欠压。
    if (g_pre_restart_hook != nullptr) {
        g_pre_restart_hook();
    }
    if (g_reboot_to_bootloader) {
        // S31 的寄存器名和 S3 不同：
        //   S3      -> RTC_CNTL_FORCE_DOWNLOAD_BOOT
        //   C6/C5   -> LP_AON_FORCE_DOWNLOAD_BOOT
        //   S31     -> LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT (SYS_CTRL bit 2)
        REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    }
    esp_restart();
}

// ---------------------------------------------------------------------------
// 免按键进下载模式：**必须认完整的 esptool 两步签名，不能只认下降沿**
// ---------------------------------------------------------------------------
// esptool 进入下载模式的签名是两步：
//
//     ① 武装：RTS 拉高、DTR 为低      (dtr=0, rts=1)
//     ② 交换：RTS 拉低、DTR 拉高      (dtr=1, rts=0)   <- 这一步触发
//
// **为什么不能只认"RTS 下降沿 + DTR 为高"**：那正是 S2/S3 的老行为，而它成立
// 的前提是 ROM 能让 USB 跨复位存活。**S31 rev0 的 ROM 里
// `chip_usb_dw_prepare_persist` 是空 stub，USB 活不过一次复位** —— 于是任何
// 普通的串口打开/关闭只要产生一个 RTS 下降沿，就会把芯片踢进下载模式、
// 然后端口直接掉线。表现就是"每次烧录完，我一去读日志，设备就没了"。
//
// 官方在 S31 上就是这么收紧的，见 esp-idf issue #18992：
//   "reboot-to-bootloader triggers only on the esptool entry signature
//    (RTS asserted with DTR clear, then swapped); ordinary terminal
//    open/close line-state edges no longer reboot the chip."
void LineStateChangedCallback(int itf, cdcacm_event_t* event) {
    (void)itf;
    const bool dtr = event->line_state_changed_data.dtr;
    const bool rts = event->line_state_changed_data.rts;

    if (rts && !dtr) {
        // 第①步：武装。
        g_saw_esptool_arm = true;
        return;
    }

    if (!rts && dtr) {
        // 第②步：交换。只有先见过武装才算数。
        if (g_saw_esptool_arm && !g_reboot_pending) {
            g_reboot_to_bootloader = true;
            g_reboot_pending = true;
            // 稍等一下再重启，让 CDC 里的数据先出完。
            esp_timer_start_once(g_restart_timer, 100 * 1000);
        }
        g_saw_esptool_arm = false;
        return;
    }

    // 其余组合（(1,1) 中间态、(0,0) 空闲）**不清除武装标记** ——
    // esptool 的时序里 (0,1) 之后会经过 (1,1) 才到 (1,0)，在中间态清掉就永远
    // 触发不了了。
}


// TinyUSB 的 RX 回调。**运行在 TinyUSB 任务上下文里，绝对不能阻塞。**
//
// 这里有三个坑，都是实测踩出来的（症状是：PC 一发 28KB 的 band，两边一起卡死）：
//
// 1. **环形缓冲满了绝对不能 `break` 掉 USB 读取。**
//    一开始写的是"满了就 break"，结果回调停止消费 USB FIFO → FIFO 也满 →
//    CDC 端点开始 NAK → **PC 侧的 write 永久阻塞**。表现就是"卡住了"。
//    正确做法：满了就**丢弃并计数**，但继续把 FIFO 读空 —— USB 必须一直被消费。
//
// 2. **每次回调尽量把 FIFO 读空。**
//    这里有个反直觉的地方：一开始我把上限设成"每次只倒 8 块"，想的是"别占着 CPU"。
//    但那样**剩余的数据可能不会再触发回调**（TinyUSB 的 CDC_EVENT_RX 是"有新数据"
//    时来的，FIFO 里已经有但没读走的部分不一定再报）—— 于是又变成卡死。
//    实际上倒满 32KB 也只是 225 轮 memcpy，微秒级，根本不是 CPU 负担。
//    上限设成 256 块（32KB）是为了万一对方发疯时有逃生口，正常情况一次就读空了。
//
// 3. **环形缓冲要放得下一条 band。** 一条 band 的 payload 是 480×30×2 = 28800 字节；
//    缓冲比它小的话，回调必然在传一半时溢出。32KB 是够一个完整的 band 再加余量。
constexpr size_t kRxChunkMax = 256;   // 单次从 FIFO 读多少
constexpr int kRxDrainPerCallback = 256;  // 上限 256×128B = 32KB，正常一次读空

void RxCallback(int itf, cdcacm_event_t* event) {
    (void)itf;
    (void)event;
    if (g_rx == nullptr) {
        return;
    }

    uint8_t buf[kRxChunkMax];
    for (int i = 0; i < kRxDrainPerCallback; i++) {
        size_t len = 0;
        if (tinyusb_cdcacm_read(kCdcItf, buf, sizeof(buf), &len) != ESP_OK || len == 0) {
            return;  // FIFO 空了，收工
        }
        if (!g_rx->Send(osal::InputBuffer(buf, len), 0)) {
            // **丢弃，但继续读。** 见上面第 1 条 —— 这里 break 会让 PC 卡死。
            g_rx_overflow = g_rx_overflow + 1;
        }
    }
}

int CdcVprintf(const char* fmt, va_list ap) {
    char buf[256];
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0 && tinyusb_cdcacm_initialized(kCdcItf)) {
        const size_t len =
            (n < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(n)
                                                : sizeof(buf) - 1;
        tinyusb_cdcacm_write_queue(kCdcItf,
                                   reinterpret_cast<const uint8_t*>(buf), len);
        tinyusb_cdcacm_write_flush(kCdcItf, 0);
    }
    return n;
}

// 用芯片 MAC 填出和 ROM 相同的序列号字符串。
void BuildSerialString() {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    snprintf(g_serial, sizeof(g_serial), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace

esp_err_t Init() {
    BuildSerialString();

    // 1. TinyUSB 设备栈。S31 是单口高速芯片，port 0 本身就是高速口。
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &kDeviceDescriptor;
    tusb_cfg.descriptor.string = kStringDescriptors;
    tusb_cfg.descriptor.string_count = kStringDescriptorCount;
    const esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        return err;
    }

    // 2. CDC-ACM + 控制线回调（免按键烧录的核心）。
    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = kCdcItf,
        .callback_rx = RxCallback,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = LineStateChangedCallback,
        .callback_line_coding_changed = nullptr,
    };
    const esp_err_t acm_err = tinyusb_cdcacm_init(&acm_cfg);
    if (acm_err != ESP_OK) {
        return acm_err;
    }

    // 2.5 接收环形缓冲 + RX 回调已经在上面注册。
    if (g_rx == nullptr) {
        g_rx = new osal::RingBuffer(65536);
    }

    // 3. 重启定时器。
    const esp_timer_create_args_t timer_args = {
        .callback = RestartTimerCallback,
        .arg = nullptr,
        .name = "usb_boot",
        .skip_unhandled_events = false,
    };
    const esp_err_t timer_err = esp_timer_create(&timer_args, &g_restart_timer);
    if (timer_err != ESP_OK) {
        return timer_err;
    }

    // 4. 注册进 VFS，之后可以当普通串口打开。
    const esp_err_t vfs_err = esp_vfs_tusb_cdc_register(kCdcItf, nullptr);
    if (vfs_err != ESP_OK) {
        return vfs_err;
    }

    ESP_LOGI(kTag, "USB-OTG CDC 就绪 (303A:%04X, SN=%s)，免按键烧录已武装",
             static_cast<unsigned>(kChipIdPid), g_serial);
    return ESP_OK;
}

bool Connected() {
    return tinyusb_cdcacm_initialized(kCdcItf);
}

size_t Write(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0 || !tinyusb_cdcacm_initialized(kCdcItf)) {
        return 0;
    }
    const size_t queued = tinyusb_cdcacm_write_queue(kCdcItf, data, len);
    tinyusb_cdcacm_write_flush(kCdcItf, 0);
    return queued;
}

size_t Write(const char* text) {
    return (text == nullptr) ? 0 : Write(reinterpret_cast<const uint8_t*>(text),
                                         strlen(text));
}

void Printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t len =
            (n < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(n)
                                                : sizeof(buf) - 1;
        Write(reinterpret_cast<const uint8_t*>(buf), len);
    }
}

void SetPreRestartHook(PreRestartHook hook) { g_pre_restart_hook = hook; }

void AttachToLogger() {
    esp_log_set_vprintf(CdcVprintf);
}

osal::InputBuffer ReceiveRx(uint32_t timeout_ms) {
    if (g_rx == nullptr) {
        return {};
    }
    return g_rx->Receive(timeout_ms);
}

void ReturnRx(osal::InputBuffer buf) {
    if (g_rx != nullptr && !buf.empty()) {
        g_rx->ReturnItem(buf);
    }
}

uint32_t RxOverflowCount() { return g_rx_overflow; }

void RebootToBootloader() {
    g_reboot_to_bootloader = true;
    REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

}  // namespace usb_console
