#!/usr/bin/env python3
"""Extract the individual app images from a full-flash backup.

Each OTA slot is carved out at its exact ESP image length (walked from the image
header, not guessed by trimming 0xFF padding), so each result is a standalone
binary that can be flashed back to its partition on its own.
"""
import argparse, hashlib, json, struct, sys
from pathlib import Path

FLASH_SIZE = 16 * 1024 * 1024
SLOTS = {"app0": (0x10000, 0x7E0000), "app1": (0x7F0000, 0x7E0000)}


def esp_image_length(buf: bytes) -> int:
    """Exact on-flash length of an ESP32 application image."""
    if not buf or buf[0] != 0xE9:
        raise ValueError("not an ESP image (magic != 0xE9)")
    seg_count = buf[1]
    hash_appended = buf[23] == 1
    off = 24  # esp_image_header
    for _ in range(seg_count):
        _addr, length = struct.unpack("<II", buf[off:off + 8])
        off += 8 + length
    off += 1                      # checksum byte...
    off = (off + 15) & ~15        # ...which sits at the end of a 16-byte block
    if hash_appended:
        off += 32                 # SHA-256 of the image
    return off


def app_desc(buf: bytes) -> dict:
    p = 0x20
    magic, = struct.unpack("<I", buf[p:p + 4])
    if magic != 0xABCD5432:
        return {}
    fld = lambda a, b: buf[p + a:p + b].split(b"\0")[0].decode("utf-8", "replace")
    return {"version": fld(16, 48), "project": fld(48, 80),
            "time": fld(80, 96), "date": fld(96, 112), "idf": fld(112, 144)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("backup_dir", type=Path)
    args = ap.parse_args()

    image = args.backup_dir / "flash-full-16mb.bin"
    data = image.read_bytes()
    if len(data) != FLASH_SIZE:
        sys.exit(f"error: {image} is {len(data)} bytes, expected {FLASH_SIZE}")

    out_dir = args.backup_dir / "apps"
    out_dir.mkdir(exist_ok=True)
    manifest = []

    for slot, (offset, size) in SLOTS.items():
        region = data[offset:offset + size]
        if not region or region[0] != 0xE9:
            print(f"{slot}: empty / no image, skipped")
            continue
        length = esp_image_length(region)
        blob = region[:length]
        desc = app_desc(blob)
        name = desc.get("project", slot).replace("/", "_")
        ver = desc.get("version", "")
        fn = f"{slot}-{name}{'-' + ver if ver else ''}.bin"
        path = out_dir / fn
        path.write_bytes(blob)
        digest = hashlib.sha256(blob).hexdigest()
        manifest.append({"slot": slot, "flash_offset": hex(offset), "file": fn,
                         "size": length, "sha256": digest, **desc})
        print(f"{slot}: {fn}")
        print(f"      {length:,} bytes  project={desc.get('project')} "
              f"version={desc.get('version')} built={desc.get('date')} {desc.get('time')}")
        print(f"      flash back with: esptool write-flash {hex(offset)} apps/{fn}")

    (out_dir / "apps.json").write_text(json.dumps(manifest, indent=2) + "\n")
    with (out_dir / "SHA256SUMS").open("w") as f:
        for e in manifest:
            f.write(f"{e['sha256']}  {e['file']}\n")
    print(f"\nwrote {out_dir}/apps.json and SHA256SUMS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
