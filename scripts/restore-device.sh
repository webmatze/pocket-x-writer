#!/usr/bin/env bash
#
# Restore a full-flash backup onto the Xteink X4 Pro.
#
#   ./scripts/restore-device.sh backups/<name> [--port /dev/cu.usbmodemXXXX] [--force-mac]
#
# Writes the whole 16 MB image back at offset 0. --flash-mode/freq/size keep is
# essential: without it esptool rewrites the bootloader header, so what lands on
# the device would not be byte-identical to the image you captured.
#
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/device-common.sh"

BACKUP_DIR=""
PORT=""
FORCE_MAC=0
while [ $# -gt 0 ]; do
  case "$1" in
    --port)       PORT="$2"; shift 2 ;;
    --force-mac)  FORCE_MAC=1; shift ;;
    *)            BACKUP_DIR="$1"; shift ;;
  esac
done

[ -n "$BACKUP_DIR" ] || die "usage: $0 backups/<name> [--port PORT] [--force-mac]"
[ -d "$BACKUP_DIR" ] || die "no such backup directory: $BACKUP_DIR"

IMAGE="$BACKUP_DIR/flash-full-16mb.bin"
[ -f "$IMAGE" ] || die "image missing: $IMAGE"

info "verifying image integrity before touching the device"
EXPECTED="$(awk '{print $1}' "$BACKUP_DIR/SHA256SUMS" 2>/dev/null | head -1)"
[ -n "$EXPECTED" ] || die "no SHA256SUMS in $BACKUP_DIR — refusing to flash an unverified image"
ACTUAL_SUM="$(sha256_of "$IMAGE")"
[ "$ACTUAL_SUM" = "$EXPECTED" ] \
  || die "checksum mismatch — image is corrupt.
  expected: $EXPECTED
  actual:   $ACTUAL_SUM"
info "checksum ok"

SIZE=$(stat -f%z "$IMAGE")
[ "$SIZE" -eq "$FLASH_SIZE_BYTES" ] || die "image is $SIZE bytes, expected $FLASH_SIZE_BYTES"

[ -n "$PORT" ] || PORT="$(detect_port)"
MAC="$(read_mac "$PORT")"
BACKUP_MAC="$(awk '/device MAC/ {print $4}' "$BACKUP_DIR/manifest.txt" 2>/dev/null || true)"

if [ -n "$BACKUP_MAC" ] && [ "$MAC" != "$BACKUP_MAC" ]; then
  warn "MAC mismatch — this backup came from a DIFFERENT device."
  warn "  backup: $BACKUP_MAC"
  warn "  device: $MAC"
  warn "The dump contains that unit's OEM hw_calib NVS namespace, which names its"
  warn "panel controller. Restoring it here can misidentify this unit's display."
  [ "$FORCE_MAC" -eq 1 ] || die "refusing. Pass --force-mac only if you know why."
  warn "--force-mac given, continuing anyway"
fi

echo
warn "About to OVERWRITE all 16 MB of flash on $PORT ($MAC)."
warn "Everything currently on the device is replaced by $BACKUP_DIR."
read -r -p "Type RESTORE to continue: " CONFIRM
[ "$CONFIRM" = "RESTORE" ] || die "aborted"

info "writing — hold the Power button throughout, release after verification"
"$ESPTOOL" --port "$PORT" --baud 921600 write-flash \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 "$IMAGE"

info "restore complete"
