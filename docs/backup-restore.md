# Device backup & restore

The Xteink X4 Pro's flash holds everything: bootloader, partition table, both
app slots, and the NVS namespace with the OEM panel calibration. A full 16 MB
dump is therefore a complete, restorable snapshot of the device.

## Take a backup

```bash
./scripts/backup-device.sh --label crosspoint
```

Auto-detects the port, reads all 16 MB (~1.5 min), hashes it, and then
**verifies it against the device** before declaring success. If verification
fails the script exits non-zero and marks the manifest `DO NOT TRUST`.

Output lands in `backups/<timestamp>-<mac>-<label>/`:

| file | purpose |
|---|---|
| `flash-full-16mb.bin` | the image |
| `SHA256SUMS` | integrity check, verified before any restore |
| `manifest.txt` | MAC, chip, esptool version, label, verification status |
| `flash-id.txt` | flash type and eFuse settings, for diagnosing restores |
| `verify.log` | output of the post-backup verification |

## Verify an existing backup

```bash
./scripts/verify-device.sh backups/<name>
```

Read-only. Checks the local checksum, then compares the image against the
device. Worth running before you flash anything experimental.

## Restore

```bash
./scripts/restore-device.sh backups/<name>
```

Three guards, in order:

1. **Checksum first** — refuses to flash an image that fails its own SHA256, so
   a corrupt file can never reach the device.
2. **MAC match** — refuses if the backup came from a different unit. The dump
   contains that unit's OEM `hw_calib` NVS namespace, which names its panel
   controller; restoring it elsewhere can misidentify the display. Override with
   `--force-mac` only if you know why.
3. **Typed confirmation** — you must type `RESTORE`.

It writes with `--flash-mode keep --flash-freq keep --flash-size keep`. Without
`keep`, esptool rewrites the bootloader header and what lands on the device is
*not* byte-identical to what you captured.

Hold the **Power button** throughout flashing; release after verification.

> **Store a copy off the repo.** `backups/` is gitignored (a 16 MB binary does
> not belong in git), so a clean checkout will not have it. Copy the directory
> somewhere durable — external disk, Time Machine, cloud.

## Recovery when USB is not an option

If the device ever stops enumerating over USB, the SD-card path is the
fallback — it is how locked units are flashed and how bricked units recover.
USB has worked on this unit (it is an unlocked xteink.com device), so this is a
safety net, not the primary route.

## Partition layout

Read from this device's own backup:

| label | type | subtype | offset | size |
|---|---|---|---|---|
| `nvs` | data | nvs | `0x9000` | `0x5000` |
| `otadata` | data | ota | `0xe000` | `0x2000` |
| `app0` | app | ota_0 | `0x10000` | `0x7e0000` (~7.9 MB) |
| `app1` | app | ota_1 | `0x7f0000` | `0x7e0000` (~7.9 MB) |
| `spiffs` | data | spiffs | `0xfd0000` | `0x14000` |
| `coredump` | data | coredump | `0xfe4000` | `0x1c000` |

**Two OTA app slots, ~7.9 MB each.** That settles the dual-boot question left
open in the plan: the layout already supports a reader in one slot and PocketX
Writer in the other, and our firmware currently uses 497 KB — roughly 6% of one
slot. No partition changes needed.

---

# Dual boot: what is in the two app slots

This device shipped to us with **both** firmwares already present. Read from its
own backup and each verified against the live device with `esptool verify-flash`:

| slot | offset | contents | build |
|---|---|---|---|
| `app0` | `0x10000` | **CrossPoint** (currently boots) | ESP-IDF 5.5.2, Feb 11 2026, 5,957,264 B |
| `app1` | `0x7f0000` | **Original Xteink firmware `xteink_app` 7.4.4** | ESP-IDF 6.0.1, Aug 27 2026, 5,586,672 B |

The CrossPoint installer did not delete the stock firmware — it moved it to the
second OTA slot. Nothing was lost.

> `app0`'s ESP app descriptor reports `project = arduino-lib-builder`; that is the
> generic project name every Arduino-framework build carries, not a different
> firmware. The binary contains "CrossPoint" 32 times.

## Extracting the individual apps

```bash
./.venv/bin/python scripts/extract-apps.py backups/<name>
```

Carves each slot at its exact ESP image length (walked from the image header —
segment table, checksum padding, appended SHA-256), not by trimming `0xFF`
padding. Results land in `backups/<name>/apps/` with `apps.json` and
`SHA256SUMS`, each independently flashable:

```bash
# put the original Xteink firmware back into its slot
./.venv/bin/esptool --port /dev/cu.usbmodem2101 write-flash \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0x7f0000 backups/<name>/apps/app1-xteink_app-7.4.4.bin
```

## Choosing which slot boots

```bash
./scripts/select-slot.py --status     # read-only
./scripts/select-slot.py --slot 1     # boot the other firmware
```

The bootloader reads two 32-byte `ota_select` entries — one per 4 KB page of the
`otadata` partition — takes the highest valid sequence number, and boots slot
`(seq - 1) % 2`. Switching means writing a higher sequence into the other entry.

The entry CRC is `crc32(seq_bytes, init=0xFFFFFFFF)`. That was **derived
empirically and checked against both entries this device shipped with** rather
than assumed, because a wrong CRC invalidates the entry and changes which
firmware boots.

The tool refuses to select a slot whose first byte is not `0xE9` (no valid ESP
image), and reads the otadata back afterwards to confirm the switch took effect.

## If a flash goes wrong

The ESP32-S3's download mode lives in ROM, so USB stays reachable even when the
application bootloops — hold **BOOT**, tap **RESET**. From there
`restore-device.sh` puts the whole 16 MB back.

## Do not let PlatformIO write the partition table

`pio run -t upload` flashes bootloader **+ partition table +** app. The default
PlatformIO table gives a 6.25 MB app slot; this device uses **7.9 MB** slots.
Uploading with the default table would rewrite the partition table and **destroy
whatever is in `app1`** — including the original firmware.

Before the first hardware upload, the project needs a `partitions.csv` that
matches this device exactly:

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x5000
otadata,  data, ota,     0xe000,   0x2000
app0,     app,  ota_0,   0x10000,  0x7e0000
app1,     app,  ota_1,   0x7f0000, 0x7e0000
spiffs,   data, spiffs,  0xfd0000, 0x14000
coredump, data, coredump,0xfe4000, 0x1c000
```

Safer still, and what we do: flash **only** the application binary to its slot
offset, leaving bootloader and partition table untouched.
