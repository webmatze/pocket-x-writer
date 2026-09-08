#!/usr/bin/env bash
#
# Verify a backup against what is actually on the device. Read-only: it writes
# nothing, it only compares. This is what turns a dump into a *trusted* backup.
#
#   ./scripts/verify-device.sh backups/<name> [--port /dev/cu.usbmodemXXXX]
#
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/device-common.sh"

BACKUP_DIR=""; PORT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    *)      BACKUP_DIR="$1"; shift ;;
  esac
done
[ -n "$BACKUP_DIR" ] || die "usage: $0 backups/<name> [--port PORT]"
IMAGE="$BACKUP_DIR/flash-full-16mb.bin"
[ -f "$IMAGE" ] || die "image missing: $IMAGE"

info "checking local image against its recorded checksum"
EXPECTED="$(awk '{print $1}' "$BACKUP_DIR/SHA256SUMS" | head -1)"
ACTUAL="$(sha256_of "$IMAGE")"
[ "$ACTUAL" = "$EXPECTED" ] || die "local image is corrupt (checksum mismatch)"
info "local checksum ok"

[ -n "$PORT" ] || PORT="$(detect_port)"
info "comparing image against device flash on $PORT"
"$ESPTOOL" --port "$PORT" --baud 921600 verify-flash \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 "$IMAGE"
info "device matches this backup byte-for-byte"
