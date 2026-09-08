# Keyboard compatibility: BLE only, and how to check

The ESP32-S3 has **no Bluetooth Classic (BR/EDR) radio**. Only Bluetooth Low
Energy HID keyboards can ever work with this device. This is silicon, not
firmware — no amount of code changes it.

## Confirmed working: Keychron K2 HE

Found by name with the HID flag at -45 dBm, connected, and typing verified on
hardware. Bonds persist in NVS, so it auto-reconnects.

The physical keyboard is a German ISO layout, which the keymap handles
correctly — the key labelled `y` sits where US layouts put `z` and therefore
sends usage 0x1D:

| key pressed | HID usage | our DE map | SDK's US map |
|---|---|---|---|
| `y` | 0x1D | **y** | z |
| `z` | 0x1C | **z** | y |
| `Shift+2` | 0x1F + shift | **"** | @ |
| key right of L | 0x33 | **ö** | ; |
| `AltGr+Q` | 0x14 + RAlt | **@** | q |
| `AltGr+E` | 0x08 + RAlt | **€** | e |

Without the German map, pressing `y` would type `z` and `Shift+2` would give
`@` — the labels on the caps would not match the screen.

A useful pre-purchase signal seen here: the K2 HE reports a 125 Hz Bluetooth
signal rate, which matches BLE's 7.5 ms connection interval (~133 Hz). Long
quoted battery life (110 h) points the same way. Neither is proof — confirm with
the macOS check below.

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

Retested in the keyboard's **Windows/Android mode** (some keyboards run a
different Bluetooth stack per host mode): still nothing. No advertisement from
the K3, and no device above -60 dBm at all during a 90 s continuous scan. It
does not speak BLE in either mode.

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
