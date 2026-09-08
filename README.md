# PocketX Writer

Distraction-free book-writing firmware for the **Xteink X4 Pro** (ESP32-S3).
Pair a BLE keyboard, write, and the chapters land on the SD card as Markdown.

No apps. No browser. No notifications.

## Status

Early bring-up. See `docs/` and the milestone plan.

- [x] **M0** Toolchain + repo + SDK integration, firmware builds for `x4pro`
- [ ] **M1** Back up stock firmware, confirm device is unlocked
- [ ] **M2** Bring-up on real hardware + simulator gate
- [ ] **M3** Typing-latency spike
- [ ] **M4** BLE keyboard
- [ ] **M5** Editor core
- [ ] **M6** Persistence (Markdown on SD)
- [ ] **M7** Book projects + chapters
- [ ] **M8** Sync to Mac

## Build

Requires Python 3.13 (PlatformIO does not yet support 3.14).

```bash
git clone --recurse-submodules <this repo>
python3.13 -m venv .venv
./.venv/bin/pip install platformio esptool
./.venv/bin/pio run -e x4pro
```

Builds on **macOS arm64** (Apple Silicon). The "ESP-IDF has no Mac ARM toolchain"
warning found in some Xteink firmware READMEs applies to the old
`espressif32@6.x` + `framework = arduino, espidf` combination, not to the
pioarduino platform used here.

## Hardware

| | Xteink X4 Pro |
|---|---|
| SoC | ESP32-S3R8, 8 MB PSRAM, 16 MB flash |
| Display | 800×480 e-ink, SSD1677 or UC8179 (batch-dependent, detected at boot) |
| Display SPI | SCLK 12, MOSI 11, CS 13, DC 18, RST 14, BUSY 6 @ 20 MHz |
| Touch | GT911 @ 0x5D (INT 4, RST 10) |
| Storage | native SDMMC, enable on GPIO5 (active-low) |
| I²C | SDA 39 / SCL 38 — BM8563 RTC @ 0x51, CW2017 fuel gauge @ 0x63 |
| Radio | BLE only — the S3 has no Bluetooth Classic |

## Credits

Built on [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) (MIT). See `NOTICE`.
