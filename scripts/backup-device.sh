#!/usr/bin/env bash
#
# Full-flash backup of the Xteink X4 Pro.
#
# Dumps all 16 MB — bootloader, partition table, every OTA app slot, and NVS
# (which holds the OEM hw_calib panel calibration and any BLE bonds). That makes
# the result a complete, restorable image of the device as it stands right now.
#
#   ./scripts/backup-device.sh [--port /dev/cu.usbmodemXXXX] [--label some-name]
#
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/device-common.sh"

PORT=""
LABEL=""
while [ $# -gt 0 ]; do
  case "$1" in
    --port)  PORT="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[ -n "$PORT" ] || PORT="$(detect_port)"
info "port: $PORT"

info "identifying device"
MAC="$(read_mac "$PORT")"
[ -n "$MAC" ] || die "could not read MAC — device not responding to esptool"
info "MAC: $MAC"

STAMP="$(date +%Y%m%d-%H%M%S)"
SAFE_MAC="${MAC//:/}"
NAME="$STAMP-$SAFE_MAC${LABEL:+-$LABEL}"
OUT_DIR="$REPO_ROOT/backups/$NAME"
mkdir -p "$OUT_DIR"
IMAGE="$OUT_DIR/flash-full-16mb.bin"

info "reading 16 MB of flash — this takes a few minutes, do not unplug"
"$ESPTOOL" --port "$PORT" --baud 921600 read-flash 0 "$FLASH_SIZE_BYTES" "$IMAGE"

ACTUAL=$(stat -f%z "$IMAGE")
[ "$ACTUAL" -eq "$FLASH_SIZE_BYTES" ] \
  || die "short read: got $ACTUAL bytes, expected $FLASH_SIZE_BYTES — backup NOT trustworthy"

info "hashing"
SUM="$(sha256_of "$IMAGE")"
echo "$SUM  flash-full-16mb.bin" > "$OUT_DIR/SHA256SUMS"

# Keep the raw tool output next to the image: it records chip revision, flash
# type and eFuse settings, which is what you want when diagnosing a restore.
"$ESPTOOL" --port "$PORT" flash-id > "$OUT_DIR/flash-id.txt" 2>&1 || true

cat > "$OUT_DIR/manifest.txt" <<EOF
PocketX Writer — device backup
==============================
created      : $(date -Iseconds)
device MAC   : $MAC
chip         : ESP32-S3
flash size   : $FLASH_SIZE_BYTES bytes (16 MB)
image        : flash-full-16mb.bin
sha256       : $SUM
esptool      : $("$ESPTOOL" version 2>/dev/null | head -1)
label        : ${LABEL:-none}
firmware     : ${LABEL:-unspecified — note what was installed when you took this}

Restore with:
  ./scripts/restore-device.sh backups/$NAME
EOF

# A dump nobody checked is not a backup. Compare it against the device now,
# while it is still plugged in, so a bad read is caught immediately rather than
# on the day you actually need to restore.
info "verifying the image against the device (read-only)"
if "$ESPTOOL" --port "$PORT" --baud 921600 verify-flash \
     --flash-mode keep --flash-freq keep --flash-size keep \
     0x0 "$IMAGE" >"$OUT_DIR/verify.log" 2>&1; then
  info "verified: backup matches device byte-for-byte"
  echo "verified    : yes ($(date -Iseconds))" >> "$OUT_DIR/manifest.txt"
else
  warn "VERIFICATION FAILED — see $OUT_DIR/verify.log"
  echo "verified    : NO — DO NOT TRUST THIS BACKUP" >> "$OUT_DIR/manifest.txt"
  die "backup could not be verified; re-run before relying on it"
fi

info "backup complete: backups/$NAME"
cat "$OUT_DIR/manifest.txt"
