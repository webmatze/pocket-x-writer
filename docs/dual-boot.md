# Dual-boot: living in two firmwares

The X4 Pro ships with two OTA slots of ~7.9 MB each, and both can hold a
complete firmware. This project uses that to keep a reader alongside the writer:

| | |
|---|---|
| `app0` @ `0x10000` | a reader — CrossPoint here, the Xteink stock firmware on an untouched device |
| `app1` @ `0x7f0000` | PocketX Writer |

Which one boots is decided by `otadata` @ `0xe000`: two 32-byte entries, and the
one with the higher sequence number wins, selecting slot `(seq - 1) % 2`.

## Going out is easy. Coming back is the problem.

Hold **Right** for 2 seconds and this firmware reboots into the other slot.

That gesture only exists here. `esp_ota_set_boot_partition()` is permanent, and
the firmware in the other slot has no reason to offer the reverse — CrossPoint
does not. **From the device alone, the switch is one way**, and two seconds on a
single key is a very small gesture for a trip you cannot end by yourself.

There are three ways back, in the order you would reach for them.

## 1. With a computer

```bash
./scripts/select-slot.py --slot 1        # back to PocketX Writer
./scripts/select-slot.py --status        # which slot boots
```

Writes a fresh `otadata` entry and reads it back to confirm. Nothing else on the
device is touched.

## 2. Without a computer: CrossPoint's own SD update

CrossPoint can install firmware from the SD card, and that is the escape hatch:

**Settings → SD Firmware Update →** pick a `.bin` from the file browser.

It validates the image (size, `0xE9` magic, chip id, XOR checksum, SHA256
trailer), writes it into the *inactive* slot and repoints `otadata` at it. About
a minute, no computer, no cable.

Our image passes CrossPoint's board guard. That guard exists so an image built
for a different board cannot be flashed onto this one, and it works by looking
for a `CROSSPOINT-BOARD-V1:<board>;` tag — but images carrying **no** tag are
explicitly allowed, and ours carries none.

> **Prepare for this before you need it.** The path only works if a
> `firmware.bin` is already on the card, and once you are stuck in the reader
> you cannot put one there. Copy it across whenever you flash:
>
> ```bash
> # with the card mounted over Ctrl+U
> cp .pio/build/x4pro/firmware.bin "/Volumes/NO NAME/pocketx-writer.bin"
> ```
>
> Give it a name you will recognise in a file browser, and **refresh it whenever
> you flash** — the card cannot be mounted while the firmware runs, so nothing
> keeps the copy in step for you. A stale copy still works; it just restores an
> older writer than the one you were using.

**Verified end to end on hardware, 2026-09-09.** Card ejected, Right held to
reach CrossPoint, Settings → SD Firmware Update, image picked, and the writer
came back on its own. Afterwards `otadata` selected `app1`, and slot 1 verified
byte-for-byte against the same `firmware.bin`: 908112 bytes, digest matched.
CrossPoint writes the image you give it, exactly.

Walk it once yourself while a cable is still at hand. A rescue path nobody has
taken is a hypothesis, the same way a backup nobody has restored is.

It costs you nothing else: the update writes an app partition and `otadata`, and
leaves `nvs` alone, so keyboard pairings and settings come through it intact —
checked after the run above, the bonded keyboard was still there.

## 3. Emergency: the reset button

If neither works — a firmware that will not boot at all — hold **Left** through
the release of the **reset** button to enter the ROM download mode and reflash.
See the README; that path is verified.

## What was ruled out

**A slot switch that undoes itself.** The idea was to switch to the reader with
`ota_state = ESP_OTA_IMG_NEW`, so the bootloader would roll back to the writer on
the next reset: out with a key hold, back with a reset, no computer, no menus.

Two measurements, and the second one kills it:

| | |
|---|---|
| Does the factory bootloader honour `ota_state`? | **Yes.** `NEW(0)` written, `ABORTED(4)` read back — a value only the bootloader writes — and it fell back to the other slot. |
| Does CrossPoint confirm itself? | **Yes.** Given a full boot, `NEW → VALID`. That cancels the rollback. |

Grepping CrossPoint for `esp_ota_mark_app_valid_cancel_rollback` finds nothing,
which is misleading: a shallow clone omits its `freeink-sdk` submodule. The
behaviour is what settles it.

Two traps worth knowing if anyone re-runs this: `esptool write-flash`
hard-resets by default, so the first boot after a write is unobservable — pass
`--after no-reset` when that boot *is* the measurement. And when writing a
one-shot entry, do not overwrite the entry that selects the slot you want to
fall back to; there has to be something left to roll back to.

## What is still possible

A **custom bootloader** that checks a key at boot and overrides the slot — hold
Right through a reset and the writer boots, whatever `otadata` says. It is the
only mechanism that works regardless of what the other firmware does, because
nothing in the other firmware gets a say.

Everything that would have made this reckless has been measured away:

| | |
|---|---|
| Secure boot / flash encryption | both disabled |
| Chip revision vs. images | device v0.2; every image needs ≥ v0.0 and caps at v0.99 or not at all |
| Flash parameters | factory and our build agree: DIO, 16 MB, 80 MHz |
| Size | our bootloader is 18736 B; 32768 B available before the partition table |
| Recovery if it fails | ROM download mode, verified: 32 KB read back byte-identical, an 8 KB write verified |

What is left is not risk but upkeep: the bootloader would have to be built from
source, and this project builds with `framework = arduino` against a *prebuilt*
one. The cheapest route to investigate first is a second env using
`framework = arduino, espidf` on the same pioarduino platform — same packages,
no new toolchain. The plan originally rejected that mode over a "no Mac ARM"
warning that turned out to apply only to the old `espressif32@6.x`.

Weigh it honestly: the bootloader buys a two-second gesture in place of a
one-minute menu. Worth building once the one-way trip actually costs you
something, not before.
