#!/usr/bin/env python3
"""Choose which OTA slot the Xteink X4 Pro boots.

The bootloader reads two 32-byte ota_select entries (one per 4 KB page of the
otadata partition), takes the one with the highest sequence number, and boots
slot (seq - 1) % 2. Switching is therefore just writing a new, higher sequence
into the other entry.

The CRC is crc32 of the 4 sequence bytes with initial value 0xFFFFFFFF --
confirmed empirically against both entries this device shipped with.

  ./scripts/select-slot.py --status
  ./scripts/select-slot.py --slot 1
"""
import argparse, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ESPTOOL = REPO / ".venv" / "bin" / "esptool"
OTADATA_OFF, OTADATA_SIZE, PAGE = 0xE000, 0x2000, 0x1000
SLOT_OFF = {0: 0x10000, 1: 0x7F0000}
BLANK = 0xFFFFFFFF


def crc(seq: int) -> int:
    return zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF


def esptool(port, *args, out=None):
    cmd = [str(ESPTOOL), "--port", port, "--baud", "921600", *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"esptool failed:\n{r.stdout}\n{r.stderr}")
    return r.stdout


def read_region(port, off, size) -> bytes:
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        tmp = Path(f.name)
    esptool(port, "read-flash", hex(off), hex(size), str(tmp))
    data = tmp.read_bytes()
    tmp.unlink()
    return data


def parse(data):
    """-> [(seq, crc_ok)] for both entries."""
    out = []
    for i in range(2):
        e = data[i * PAGE:i * PAGE + 32]
        seq, = struct.unpack("<I", e[:4])
        stored, = struct.unpack("<I", e[28:32])
        out.append((seq, seq != BLANK and stored == crc(seq)))
    return out


def active_slot(entries):
    valid = [(s, i) for i, (s, ok) in enumerate(entries) if ok]
    if not valid:
        return None, None
    seq, idx = max(valid)
    return (seq - 1) % 2, seq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem2101")
    ap.add_argument("--slot", type=int, choices=(0, 1))
    ap.add_argument("--status", action="store_true")
    a = ap.parse_args()

    data = read_region(a.port, OTADATA_OFF, OTADATA_SIZE)
    entries = parse(data)
    cur, cur_seq = active_slot(entries)

    for i, (seq, ok) in enumerate(entries):
        shown = "blank" if seq == BLANK else str(seq)
        print(f"  entry{i}: seq={shown:<10} crc={'ok' if ok else 'invalid/blank'}")
    print(f"  -> currently boots app{cur} (seq {cur_seq})" if cur is not None
          else "  -> no valid otadata; bootloader falls back to the first app")

    if a.status or a.slot is None:
        return 0
    if a.slot == cur:
        print(f"\nalready booting app{a.slot}; nothing to do")
        return 0

    # Refuse to point the bootloader at a slot with no image in it.
    head = read_region(a.port, SLOT_OFF[a.slot], 0x100)
    if head[0] != 0xE9:
        sys.exit(f"error: app{a.slot} holds no valid ESP image (magic 0x{head[0]:02X}) "
                 f"-- refusing to make it the boot slot")

    highest = max(s for s, ok in entries if ok) if any(ok for _, ok in entries) else 0
    seq = highest + 1
    if (seq - 1) % 2 != a.slot:
        seq += 1
    target = 0 if entries[0][0] != highest else 1   # write into the other entry

    print(f"\nsetting app{a.slot} as boot slot (seq {seq} into entry{target})")
    blob = bytearray(data)
    rec = struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<II", BLANK, crc(seq))
    blob[target * PAGE:target * PAGE + 32] = rec

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        tmp = Path(f.name)
    tmp.write_bytes(bytes(blob))
    esptool(a.port, "write-flash", "--flash-mode", "keep", "--flash-freq", "keep",
            "--flash-size", "keep", hex(OTADATA_OFF), str(tmp))
    tmp.unlink()

    after = parse(read_region(a.port, OTADATA_OFF, OTADATA_SIZE))
    now, now_seq = active_slot(after)
    if now != a.slot:
        sys.exit(f"error: readback says app{now} still boots -- switch did NOT take")
    print(f"verified: device now boots app{now} (seq {now_seq})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
