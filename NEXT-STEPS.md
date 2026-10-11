# Where we stopped / where to resume

## Current state

The firmware and the DeepSeek Harness plugin are both written and build cleanly.
Everything is committed. **The only blocker is the board itself**: it keeps
dropping off the USB bus and currently does not enumerate at all
(`ESP32-S31: 0` in the ioreg tree, no `/dev/cu.usbmodem*`).

Nothing is holding the port (the plugin is unloaded and `lsof` shows zero fds),
so this is a hardware-side problem, not a software one.

## What was just changed and is NOT yet flashed

Commit `0d006a7` splits USB CDC into two ports:

    CONFIG_TINYUSB_CDC_COUNT=2

    port 0 (kFlashItf) -- control-line callback only. A flasher talks to this
                          one; button-free flashing is unchanged.
    port 1 (kCdcItf)   -- the display protocol. No line-state callback, and a
                          different /dev node.

This fixes two separate, measured problems:

1. Opening a macOS serial node asserts DTR/RTS. On the flashing port those are
   the reset signal, so the plugin opening the port to look for the board kicked
   the chip out of ROM download mode.
2. An open port is an exclusively held port. `lsof` showed three DeepSeek fds on
   `/dev/cu.usbmodem21201` and esptool failed with "Serial data stream stopped".

## What tonight added

- `flash-rom.sh` referenced `mosaico-fluidbox.bin` (leftover from the previous
  project) instead of `mosaico-deck.bin`, so the ROM-mode recovery path -- the
  only one that works when the board will not enumerate normally -- always
  bailed out. Fixed.
- **A restart does not necessarily release the serial port.** After restarting
  Harness at 03:48 the port was still held, and `ps` showed a leftover process
  from the 03:24 instance (PID 33484) that had never been reaped. That process
  was still alive and still holding `/dev/cu.usbmodem21201`. It had to be killed
  explicitly; nothing else freed the port. Worth checking with
  `lsof /dev/cu.usbmodem*` and `ps -o lstart= -p <pid>` whenever the port is
  busy, because the owner may not be the session you think it is.
- The board now enumerates only intermittently: it appears for a few seconds,
  flashing fails with "port is busy" or a pySerial ioctl error, and then it
  disappears again (`ioreg` back to 0). It never stays up long enough for
  esptool to complete.

## Resume here

1. Get the board enumerating again: unplug USB, wait 10s, replug. If that does
   not work, try another cable (data, not charge-only) and another port (direct,
   not through a hub).
2. `ls /dev/cu.usbmodem*` -- with the dual-CDC firmware flashed it should show a
   **pair** of nodes (`...201` and `...203`). Before flashing you will see one.
3. `./flash.sh` -- flash the dual-CDC firmware.
4. **Verify button-free flashing still works** (`./flash.sh` twice more). This is
   the fragile part and the change touched that file. `./tools/check_port.sh`
   guards the two-step signature; it must print `[ok]` for every line.
5. Point the plugin at the **second** node of the pair (the one with the higher
   suffix). It must do nothing when no pair is present -- that is what makes it
   safe to leave running while flashing.

## Still unverified

- **Automatic status sync.** The plugin's tool calls work (`deck_show` responds),
  but the screen was never observed following the agent, because the board has
  not been reachable. If it does not move, the next step is to log every
  lifecycle event the plugin actually receives -- the suspects are the event
  names and whether `ctx` is agent-scoped.

## Known-good reference points

    ./tools/check_port.sh     button-free flashing guard, must be all [ok]
    ./install-plugin.sh       copies the plugin and runs its load self-test
    ./flash.sh                app is running; knocks first
    ./flash-rom.sh            already in ROM mode; does NOT knock

The plugin's load self-test (`plugin/selftest.mjs`, run by install-plugin.sh)
must print `safe to restart DeepSeek Harness` before any restart.
