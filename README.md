# PocketX Writer

Distraction-free writing firmware for the **Xteink X4 Pro** e-reader. Pair a
Bluetooth LE keyboard, write, and your chapters land on the SD card as plain
Markdown.

No apps. No browser. No notifications. A 4.3" e-ink screen, a keyboard, and
days of battery.

Built for writing books on the move — but it is a general-purpose text editor,
and nothing about it is specific to fiction.

> **The on-device UI is German**, as is the keyboard layout, since that is what
> the author writes in. Both are a handful of strings and one lookup table away
> from any other language; see `src/core/de_keymap.*` and the status line in
> `src/main.cpp`.

```
Kap. 3/7   1240 Worte   Ziel 62%   84%   gespeichert
─────────────────────────────────────────────────────
Der Hahn krähte, lange bevor irgendjemand im Haus
wach sein wollte. Marie lag noch einen Moment mit
offenen Augen da und hörte zu, wie der Morgen▮
```

---

## ⚠️ Read this before you buy a keyboard

**The ESP32-S3 has no Bluetooth Classic radio.** Only **Bluetooth LE (BLE) HID**
keyboards can ever work. This is silicon, not software — no firmware change
alters it.

Spec sheets are not reliable here. A keyboard advertised as "Bluetooth 5.1" can
still speak the Classic HID profile. During development a **Keychron K3** turned
out to be Classic-only and could not connect at all, while a **Keychron K2 HE**
works perfectly.

**Check before buying**, with no special tools:

1. Pair the keyboard with a Mac.
2.  → About This Mac → System Report → **Bluetooth**.
3. Find the keyboard and read its **Services** line.
   - Contains `ACL`, no `GATT` → **Bluetooth Classic. Will not work.**
   - Lists LE / GATT services → BLE. Should work.

See [`docs/keyboard-compatibility.md`](docs/keyboard-compatibility.md) for the
full evidence.

| Keyboard | Result |
|---|---|
| Keychron K2 HE | ✅ works (German ISO tested) |
| Keychron K3 | ❌ Bluetooth Classic — cannot work |

---

## What it does

- **Write** with a BLE keyboard, in German or English
- **Full German T1 layout**: QWERTZ, umlauts, ß, the German shift row, AltGr
  symbols (`@ € { [ ] } \ ~ | µ`), and dead keys that compose (`´` + `e` → `é`)
- **Edit**: cursor navigation, word-wise movement, undo, selection, clipboard
- **Book projects**: chapters as `chapters/NN-slug.md`, word count, daily goal
- **Never lose work**: verified atomic saves, autosave on pause
- **Sync**: the SD card mounts on your computer over the same USB-C cable
- **Frontlight**, battery indicator, deep sleep, real file timestamps

### Keyboard shortcuts

| | |
|---|---|
| `Ctrl+S` | save now |
| `Ctrl+Z` | undo |
| `Ctrl+A` / `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | select all, copy, cut, paste |
| `Shift`+arrows | select · with `Ctrl`: by word |
| `Ctrl`+`←`/`→` | move by word |
| `Ctrl+Home` / `Ctrl+End` | jump to the start / end of the chapter |
| `Ctrl+Backspace` | delete previous word |
| `PageUp` / `PageDown` | previous / next chapter |
| `Ctrl+N` | new chapter |
| `Ctrl+U` | mount the SD card on your computer |

`Cmd` works wherever `Ctrl` does, so the shortcuts survive a Mac/Windows mode
switch on the keyboard.

### Device buttons

| | |
|---|---|
| **Left** (short press) | cycle frontlight: off → 12 → 30 → 60 → 100% |
| **Right** (hold 2 s) | reboot into the other firmware slot — **one way**, see below |
| **Power** (short press) | sleep now — saves first, then shows a sleep screen |
| **Power** (while asleep) | wake |
| **Reset** (small button on the top edge) | hardware reset — see below |

Switching slots is **one way from the device**. `esp_ota_set_boot_partition()`
is permanent, and hold-Right-to-switch is a feature of *this* firmware, so
whatever is in the other slot has no reason to offer the reverse — CrossPoint
does not.

Ways back, in the order you would reach for them: `scripts/select-slot.py
--slot 1` from a computer; **CrossPoint's own Settings → SD Firmware Update**,
which installs a `.bin` from the card and needs no computer at all (verified on
hardware) — so keep a copy of `firmware.bin` on the card, because once you are
stuck in the reader you cannot put one there; or, if nothing boots, the reset button and the ROM
download mode below.

Full picture, including the one-shot-boot idea that measurement ruled out:
[`docs/dual-boot.md`](docs/dual-boot.md).

### The reset button is your way out

The small button on the top edge is a **hardware reset on the EN line**, not a
software reboot. Verified on hardware: `esp_reset_reason()` reports
`ESP_RST_POWERON` (1), and USB drops and re-enumerates — where an `esptool`
DTR reset reports `ESP_RST_USB` (11) and keeps the port.

That distinction matters, because it resets the **RTC domain** as well. Some
chip state lives there and survives `esp_restart()`, a firmware switch, and even
unplugging the cable once the power latch is asserted — the USB pad routing left
behind by TinyUSB is the example that bites. When the device is running fine but
has vanished from USB entirely, this button is the only thing short of a flat
battery that clears it.

Holding **Left** (GPIO0, the boot strap) while pressing it drops the chip into
the ROM download mode — the recovery path when a firmware fails to boot at all.
Verified on hardware:

```
rst:0x15 (USB_UART_CHIP_RESET),boot:0x23 (DOWNLOAD(USB/UART0))
waiting for download
```

From there, with no application running, `esptool` reads and writes flash
normally: 32 KB of bootloader and partition table read back byte-identical to
the backup, and an 8 KB write verified with a matching digest. That is the whole
recovery path, proven end to end.

**It looks exactly like nothing happening** — no screen change, and the ROM's own
banner goes out before USB re-enumerates, so the host never sees it. The way to
tell is `esptool --before no-reset`: if it connects, the chip is already
listening. Leave with a press of the reset button; a reset over USB lands back
in download mode.

Hold Left through the *release* of the reset button, not just the press: GPIO0 is
sampled as reset is released.

Every boot now prints why it happened:

```
[boot] reset reason 1: power-on / EN pin -- RTC domain reset too
```

---

## Files on the card

```
/books/<book>/chapters/01-erstes-kapitel.md
                       02-zweites-kapitel.md
```

Plain UTF-8 Markdown. Titles are transliterated to ASCII the way German does it
(`Über den Dächern` → `ueber-den-daechern`), because SdFat cannot reopen a
non-ASCII long filename — a chapter saved under one would be lost to the next
boot.

Each save leaves a `.bak` beside the file.

---

## Hardware

| | Xteink X4 Pro |
|---|---|
| SoC | ESP32-S3R8 — 8 MB PSRAM, 16 MB flash |
| Display | 800×480 e-ink, 4.3", ~219 PPI |
| Controller | SSD1677 **or** UC8279, depending on production batch — detected at boot |
| Touch | GT911 (present, deliberately unused) |
| Frontlight | dual warm/cold |
| Storage | microSD (native SDMMC) |
| Radio | Wi-Fi 2.4 GHz, **BLE only** — no Bluetooth Classic |
| Battery | 1100 mAh |

Other FreeInk-supported devices are not currently targeted, but the hardware
layer supports many; see [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk).

---

## Back up your device first

**Do this before flashing anything.** The Xteink stock firmware is not available
for download. If you overwrite it without a copy, it is gone.

```bash
./scripts/backup-device.sh --label stock
```

Reads all 16 MB, hashes it, and then **verifies the image against the device**
before reporting success. A dump nobody checked is not a backup.

```bash
./scripts/verify-device.sh backups/<name>     # read-only re-check
./scripts/restore-device.sh backups/<name>    # guarded restore
./.venv/bin/python scripts/extract-apps.py backups/<name>   # carve out each app slot
./scripts/select-slot.py --status             # which firmware boots
```

Restore refuses to run on a checksum mismatch, refuses a backup from a different
device (the dump carries that unit's OEM panel calibration), and asks you to
type `RESTORE`.

**Store a copy off the repo** — `backups/` is gitignored.

Full details: [`docs/backup-restore.md`](docs/backup-restore.md).

---

## Build

Requires **Python 3.13** (PlatformIO does not support 3.14 yet). Builds on macOS
arm64, Linux and WSL.

```bash
git clone --recurse-submodules https://github.com/webmatze/pocket-x-writer.git
cd pocket-x-writer
python3.13 -m venv .venv
./.venv/bin/pip install platformio esptool
./.venv/bin/pio run -e x4pro
```

> The "ESP-IDF has no Mac ARM toolchain" warning found in some Xteink firmware
> READMEs applies to the old `espressif32@6.x` + `framework = arduino, espidf`
> combination. This project uses the pioarduino platform and builds natively on
> Apple Silicon.

### Flash

```bash
./scripts/flash-app.sh --slot 1
```

Writes **only** the application binary to one OTA slot and verifies it.
Bootloader and partition table are never touched, so whatever lives in the other
slot survives — you can keep a reader firmware alongside this one and switch
with a 2-second hold of the Right button.

**NVS is not touched either**, so keyboard pairings and the frontlight setting
survive every firmware update — verified across an update installed from the SD
card. What *would* clear them is a full-image `restore-device.sh` (the dump
carries the NVS of the day it was taken), an `esptool erase-flash`, or the `f`
serial command.

Deliberately **not** `pio run -t upload`: that flashes a partition table too, and
PlatformIO's default table would rewrite the device's and destroy the other slot.

### Tests

```bash
./.venv/bin/pio test -e native
```

95 tests, ~3 seconds, no hardware required.

---

## How it is put together

The parts where correctness actually bites live in `src/core/`, free of Arduino,
the SDK and the display, so they build for the host and run under unit tests:

| | |
|---|---|
| `de_keymap` | HID usage + modifiers → UTF-8, German T1 with dead keys |
| `document` | text, cursor, selection, undo |
| `text_render` | UTF-8 rendering into a 1-bit framebuffer, word wrap, frame diffing |
| `project` | chapter filenames, word counting, goal progress |
| `slug` | title → safe ASCII filename |

Hardware access sits above that: `src/main.cpp` for the editor loop and
`src/storage.*` for the SD card.

Writing it this way paid off immediately — the first keymap test run failed
because the *test* used wrong HID usages, which on-device would have looked like
a keymap bug.

### Display refresh

E-ink partial updates leave residual charge, which shows as ghosting and makes
fresh characters start faint. The firmware:

- compares each frame against the previous one and refreshes **only the changed
  rectangle**, usually one line and often just the width of a character
- accumulates repainted **area**, not refresh count, and scrubs sooner the more
  was repainted — clearing a full-page selection cleans up in about a second,
  ordinary typing waits six
- runs a full flash on chapter switches, where the whole page changes anyway

Measured on hardware: a whole-panel partial takes ~554 ms on this glass, so
per-keystroke feedback is ~270 ms typical and ~477 ms worst case with
coalescing. Input is never the bottleneck — the keyboard stays responsive.
See [`docs/panel-and-latency.md`](docs/panel-and-latency.md).

---

## Known limitations

- **Display latency is a hardware floor.** ~270 ms from keystroke to visible
  character. Typing feels fine; the display trails.
- The **scrub flash** inverts the screen briefly when clearing ghosting. That
  inversion *is* the cleanup.
- **German typographic quotes** (`„ "`), en/em dashes and the ellipsis are
  outside the rasterised character range. Straight quotes work; convert on
  export.
- **Runs of backspaces** do not coalesce into a single undo step.
- `.bak` files sit visibly in the `chapters/` directory.
- **Touch is deliberately unused.** This is a keyboard-driven writing device; a
  touchscreen adds accidental input where the hands rest and buys nothing the
  keyboard does not do better.

---

## Upstream fixes

Two bugs found in the FreeInk SDK while building this. The `vendor/freeink-sdk`
submodule therefore points at a [fork](https://github.com/webmatze/freeink-sdk)
carrying them on the `uc8279x4-display-window` branch; the patches are also in
[`patches/`](patches/) so the changes can be read without checking anything out.
Both are worth offering upstream:

- **`Uc8279X4Driver::displayWindow()` was missing entirely**, so every windowed
  request silently repainted the whole panel.
- **The BLE page-turner fallback ran on ordinary keyboard frames.** Its guard
  accepted any device exposing a Consumer Control page — which is nearly every
  keyboard with media keys — turning every key *release* into a phantom
  keypress.

---

## Credits

Built on the [**FreeInk SDK**](https://github.com/Free-Ink/freeink-sdk) (MIT),
which provides the display drivers, panel auto-detection, input, SD, frontlight,
power and BLE HID host. Without it this would have been months of
reverse-engineering rather than a hardware layer that already knew this device.

Design and behaviour references, both MIT:
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) and
[MicroSlate](https://github.com/Josh-writes/microslate-firmware).

Fonts are [Noto Sans](https://fonts.google.com/noto) under the SIL Open Font
License 1.1, rasterised over U+0020–U+00FF.

## License

MIT — see [LICENSE](LICENSE) and [NOTICE](NOTICE).

---

*This firmware writes to your device's flash. Every feature here was verified on
real hardware, but it is young software and you are flashing your own device at
your own risk. Take the backup first.*
