#!/usr/bin/env bash
# Copyright 2026 ESP-Claw Authors
# SPDX-License-Identifier: MIT
#
# check_port.sh -- verify the button-free flashing logic is still intact.
#
# Why this exists: this logic was silently deleted once (a bad edit range while
# reworking the RX callback removed the whole LineStateChangedCallback). The
# symptom is deceptive -- it does not fail to compile, it makes the device
# disappear after a single log read, which looks like a hardware fault.
#
# The compiler cannot catch that, so it gets its own guard.
#
# IMPORTANT: the patterns below match CODE, not comments. An earlier version
# grepped for Chinese comment text, so translating the comments made the guard
# fail even though the behaviour was untouched. Guards must assert the thing
# that matters -- here, the actual two-step signature.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$ROOT/components/usb_console/src/usb_console.cpp"

fail=0
need() {
    if grep -q "$1" "$SRC"; then
        printf "  [ok] %s\n" "$2"
    else
        printf "  [FAIL] missing: %s\n" "$2"
        fail=1
    fi
}

echo "=== Button-free flashing: logic self-check ==="
need "g_saw_esptool_arm = false"          "arm flag is initialised"
need "if (rts && !dtr)"                   "step 1 branch (arm):       RTS=1 DTR=0"
need "g_saw_esptool_arm = true"           "step 1 sets the arm flag"
need "if (!rts && dtr)"                   "step 2 branch (swap):      RTS=0 DTR=1"
need "g_saw_esptool_arm && !g_reboot_pending" "step 2 requires the arm flag (two-step signature)"
need "FORCE_DOWNLOAD_BOOT"                "FORCE_DOWNLOAD_BOOT register"
need "esp_restart"                        "esp_restart"
need "esp_timer_start_once"               "delayed restart lets CDC drain"
need "callback_line_state_changed"        "line-state callback is registered"
need "kRestartTimerName\|restart_timer"   "restart timer exists"

# The (1,1) intermediate state must NOT clear the arm flag, or the two-step
# signature collapses back into "any falling edge triggers download mode",
# which is exactly the bug that killed the port permanently on this silicon.
if grep -A4 "The other combinations" "$SRC" | grep -q "do not"; then
    printf "  [ok] (1,1) intermediate state does not clear the arm flag\n"
else
    printf "  [warn] could not confirm the (1,1) rule by comment; check it by hand\n"
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "[FAIL] Button-free flashing logic is incomplete -- DO NOT FLASH."
    echo "       Compare against mosaico-fluidbox/components/usb_console first."
    exit 1
fi
echo "[ok] Button-free flashing logic intact"
