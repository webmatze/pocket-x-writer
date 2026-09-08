# Keyboard compatibility: BLE only, and how to check

The ESP32-S3 has **no Bluetooth Classic (BR/EDR) radio**. Only Bluetooth Low
Energy HID keyboards can ever work with this device. This is silicon, not
firmware — no amount of code changes it.

## The Keychron K3 (this unit) does NOT work

Tested on hardware, 2026-09-08. Three independent findings, all pointing the
same way:

| evidence | result |
|---|---|
| macOS System Report | `Dienste: 0x800020 < HID ACL >` — **ACL is a Classic link**. The Mac's own controller lists `GATT` for its LE support; the K3 shows none. |
| BLE scan, 45 s continuous, keyboard in pairing mode | `DC:2C:26:26:B4:4F` **never appeared in a single advertisement** |
| direct BLE connect to that address (both address types) | `connect timed out after 15s` |

Crucially, the same scan that missed the keyboard **did** flag a neighbour's
Sky Q remote as `HID`. Our detection works; the keyboard is simply not on BLE.

The keyboard reports manufacturer ID `0x05AC` and product ID `0x0250` — it
presents itself as an Apple keyboard, which is how Keychron gets seamless macOS
support. Firmware 1.0.1.

> Note: the MicroSlate firmware README lists "Keychron K3" as tested. Either that
> refers to a different revision (the K3 Pro uses a different chipset) or the
> claim does not hold for all units. Do not treat a model name in someone's
> README as proof that a specific keyboard speaks BLE.

## How to check a keyboard BEFORE buying

The reliable test needs no special tools:

1. Pair it with a Mac, then open  → About This Mac → System Report →
   **Bluetooth**.
2. Find the keyboard and read its **Dienste / Services** line.
   - Contains `ACL` and no `GATT` → **Classic. Will not work.**
   - Lists LE/GATT services → BLE. Should work.

A second, weaker signal: a keyboard advertised as working with iPads *and*
low-power/long-battery-life is usually BLE. "Bluetooth 3.0" in the spec sheet
means Classic. "Bluetooth 4.0+/5.x LE" is what you want — but the spec sheet
often says 5.1 while the keyboard still uses the Classic profile, so prefer the
macOS check.

## Options when the keyboard is Classic

1. **A different, confirmed-BLE keyboard.** Verify with the macOS check above
   rather than trusting a model list.
2. **USB host.** The ESP32-S3 has a native USB OTG peripheral and could act as a
   USB host for a wired keyboard — the K3 has a USB-C cable already. Two
   obstacles: the build currently uses `ARDUINO_USB_MODE=1` (hardware
   USB-Serial/JTAG) while TinyUSB host needs OTG mode, and the device must
   supply 5 V on VBUS, which an e-reader has no reason to do. Needs
   investigation before it can be promised.
