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

## Hardware bring-up: done

Verified on the board (ESP32-S31 rev v0.0, MAC `1c:29:04:d0:97:d8`) against
ESP-IDF v6.1.0:

1. ~~Get the board enumerating again~~ -- it enumerates. A replug brought it back,
   and it stays up for as long as nothing opens the port.
2. ~~`ls /dev/cu.usbmodem*` shows a pair~~ -- **confirmed**, and the interface
   names pin the mapping down:

       /dev/cu.usbmodem111201   itf 0, "FluidBox CDC", class 2   = kFlashItf (flashing)
       /dev/cu.usbmodem111203   itf 1, class 10 (CDC data)      = kCdcItf (display)

   The node suffix follows the *control* interface number, so the higher suffix is
   the data port. `pickDataPort()` takes it; `flash.sh`'s `head -1` takes the
   flashing port. The two scripts no longer collide.
3. ~~`./flash.sh`~~ -- flashed, and the app runs (boot animation seen on the panel).
4. ~~Verify button-free flashing~~ -- **done twice**, both clean, and the pair comes
   back on its own after each reset.
5. ~~Point the plugin at the **second** node of the pair~~ -- **done in code**:
   `pickDataPort()` in `plugin/lib/device.js` takes the higher node of a pair and
   returns nothing when no pair is present. Pinned by
   `node plugin/test-port-selection.mjs`, which needs no hardware.

### What bit us on the way

- **The first flash, done from a stale ROM-mode enumeration, produced one node
  instead of two -- and the app was not running.** The chip had been sitting in ROM
  download mode; after flashing, macOS kept the ROM's *one-interface* configuration
  until the board was replugged. The cause is the design: the firmware copies the
  ROM's device descriptor byte for byte (same VID/PID/serial/strings), so the host
  has no reason to think the configuration changed. After a replug the pair appeared,
  and every later button-free flash restored it immediately. **Single node after
  flashing -> replug once.**
- `check-board.sh` crashed on its *success* path: macOS ships bash 3.2, which folds a
  following multibyte character into the variable name, so `$MAC）` is read as the
  variable `MAC）` -- and under `set -u` that is a hard "unbound variable" error. The
  one line that reports the answer the script exists to give was the one line that
  could not run. Braces (`${MAC}`) delimit the name.
- **The IDF python env path must not be hardcoded.** Its directory name embeds the
  python version (`idf6.1_py3.12_env` here, `idf6.1_py3.11_env` on the author's
  machine). Hardcoded, the scripts silently fall back to the system `python3`, which
  has pyserial but **not esptool** -- so the failure surfaces much later, inside the
  wrapper's import. `flash.sh`, `flash-rom.sh`, `check-board.sh` and
  `reset-to-download.sh` now glob `~/.espressif/python_env/idf*_env` instead.
- **A board reset wedges the plugin, and it cannot un-wedge itself.** When the chip
  re-enumerates while the port is open, the fd keeps pointing at a device that no
  longer exists. Every write then fails (EBADF/ENXIO) **while `connected` still
  reports true**, because that getter only checks `fd !== null` -- so
  `deck_connect` early-returns and refuses to reopen either. Observed live: the
  plugin kept rendering on every tool call (a fresh `render.py` each time) and threw
  every frame away, reporting "no change"; only a host restart brought it back.
  `_writeAll` now closes the deck on any non-EAGAIN write error, which makes
  `connected` honest so `deck_connect` can recover it. Pinned by
  `node plugin/test-dead-fd.mjs` (no hardware: a dead fd is simulated with -1).
  Worth knowing when testing: **anything that resets the board -- including a second
  process opening the port while the plugin holds it -- lands you here.**
- **`install-plugin.sh` never updated the code on a re-install.** `cp -R src dst`
  copies *into* `dst` once it exists, so the second run produced `lib/lib/` and left
  the real `lib/` on the previous version -- and the load self-test immediately after
  it then validated that stale copy, printing "safe to restart" for code the host
  would never load. The script's own advice ("re-run this after editing the source")
  silently did nothing. It now clears the destination first, which also drops files
  that no longer exist in the repo.

## Verified end to end

The panel follows the agent. Confirmed on the board with the plugin loaded into a
live Harness:

- The plugin holds **`/dev/cu.usbmodem111203`** (the data port) while
  `/dev/cu.usbmodem111201` (the flashing port) stays free -- so button-free flashing
  works *while the plugin is running*. That is the entire point of the two-port split,
  and it is now measured rather than argued.
- It paints from host events alone, with no tool call from the model. The write counter
  on its fd climbs with every tool call (~10 bands each), and one small update costs
  **2 bands** -- exactly the `改一行字 | 2 条 band` row in the README's table.
- `deck_speak` returned `spoke 3.7s of audio in 3569ms`: the `say` + `ffmpeg` pipeline
  and the 0.97 pacing constant both hold on real hardware.
- The panel shows the status page, and the audio is audible.

Note for whoever reads this next: `deck_show` answering `no change` is **not** a fault
signature. `tools/pre-execute` fires the plugin's own paint first, so by the time the
tool body runs, `painting` is set and the explicit paint is skipped. Measure the fd's
write counter instead of trusting that string.

### Recovering a wedged plugin without restarting Harness

`install-plugin.sh` does not trigger a reload -- the host watches the *profile*, not
the plugin's source, so re-running the installer changes nothing it cares about.
Editing `~/.dsh/profiles/<profile>/package.json` does: removing the bundle row unloads
the plugin (its `ctx.effect` disposer closes the port), and putting the row back loads
it fresh, which reconnects. That is how the instance wedged by the reset above was
recovered without a restart. With the `_writeAll` fix in place `deck_connect` should
now handle it directly, so treat this as the fallback.

## Known-good reference points

    ./tools/check_port.sh              button-free flashing guard, must be all [ok]
    ./install-plugin.sh                copies the plugin and runs its load self-test
    node plugin/test-port-selection.mjs  port choice; needs no hardware
    node plugin/test-dead-fd.mjs       dead-fd recovery; needs no hardware
    <dsh python> plugin/test_render_layout.py   panel layout/font; needs no hardware
    ./flash.sh                         app is running; knocks first
    ./flash-rom.sh                     already in ROM mode; does NOT knock
    idf.py build                       needs ESP-IDF v6.1; the scripts find its python env

The plugin's load self-test (`plugin/selftest.mjs`, run by install-plugin.sh)
must print `safe to restart DeepSeek Harness` before any restart.
