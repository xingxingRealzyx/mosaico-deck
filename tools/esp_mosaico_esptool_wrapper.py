#!/usr/bin/env python3
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# =============================================================================
#  esp_mosaico_esptool_wrapper.py —— 烧录后清掉 FORCE_DOWNLOAD_BOOT 再复位
#
#  要解决的问题
#  ----------
#  ESP32-S31 的 ROM 里有一个 `LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT` 位（LP_SYSTEM
#  SYS_CTRL 寄存器的 bit 2）。置位之后，**下一次复位会直接回到 ROM 下载模式**，
#  而不是去跑应用固件。
#
#  本工程的 `usb_console` 组件就是靠置这个位来实现"免按键进下载模式"的：收到
#  DTR/RTS 时序后置位 + `esp_restart()`。
#
#  问题是 **esptool 的 `--after hard-reset` 不会清这个位**。于是：
#
#      免按键进下载模式（置位）-> 烧录 -> hard reset -> ROM 看到位还立着
#      -> 又进下载模式 -> 应用永远不跑
#
#  表现就是"烧完了但板子不工作"，而且 `ioreg` 里看到的还是 ROM 的产品名
#  `ESP32-S31`（连字符），很容易误判成"板子坏了"。
#
#  这个包装器做的事
#  --------------
#  给 esptool 的 `ESP32S31ROM.hard_reset` 打个猴子补丁：在真正复位之前，先往
#  `LP_SYSTEM_SYS_CTRL_REG` 写一次，把 FORCE_DOWNLOAD_BOOT 位清掉。
#
#  只在该目标是 USB-OTG 连接时才做（`uses_usb_otg()`）—— USB Serial/JTAG 那条
#  路不经过这个位。
#
#  来源
#  ----
#  移植自 espressif/esp-brookesia 的
#  `hal/brookesia_hal_boards/components/mosaico/esp_mosaico_usb_console/tools/
#   esp_mosaico_esptool_wrapper.py`（Apache-2.0）。
#  本文件保留了同样的寄存器地址与掩码，只把注释换成中文并补上"为什么"。
#
#  用法
#  ----
#      python tools/esp_mosaico_esptool_wrapper.py esptool \
#          --chip esp32s31 -p /dev/cu.usbmodem21201 --before no-reset \
#          --after hard-reset write-flash @flash_args
#
#  也就是把原来的 `python -m esptool` 换成
#  `python tools/esp_mosaico_esptool_wrapper.py esptool`，其余参数原样。
# =============================================================================

import functools
import sys

# LP_SYSTEM 的 SYS_CTRL 寄存器。地址来自官方实现，不要凭感觉改。
LP_SYSTEM_SYS_CTRL_REG = 0x20700008
LP_SYSTEM_FORCE_DOWNLOAD_BOOT_MASK = 1 << 2


def _patch_esp32s31_hard_reset() -> None:
    """把 ESP32S31ROM.hard_reset 换成"先清位、再复位"的版本。"""
    from esptool.targets.esp32s31 import ESP32S31ROM

    original_hard_reset = ESP32S31ROM.hard_reset

    @functools.wraps(original_hard_reset)
    def hard_reset_with_force_download_clear(self) -> None:
        # 只有走 USB-OTG 时才需要清；USB Serial/JTAG 不经过这个位。
        if self.uses_usb_otg():
            try:
                self.write_reg(
                    LP_SYSTEM_SYS_CTRL_REG,
                    0,
                    LP_SYSTEM_FORCE_DOWNLOAD_BOOT_MASK,
                )
            except Exception:
                # 复位的一瞬间端口本来就可能消失，这里失败不算问题 ——
                # 保持 esptool 原有的复位行为即可，不要因为它把烧录判成失败。
                pass
        original_hard_reset(self)

    ESP32S31ROM.hard_reset = hard_reset_with_force_download_clear


def main() -> None:
    # 用法和 `python -m esptool` 一样，只是前面多一个 "esptool" 参数。
    if len(sys.argv) < 2 or sys.argv[1] != "esptool":
        raise SystemExit("expected 'esptool' as the first argument")
    del sys.argv[1]

    import esptool

    _patch_esp32s31_hard_reset()
    esptool._main()


if __name__ == "__main__":
    main()
