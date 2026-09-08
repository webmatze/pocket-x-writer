#!/usr/bin/env bash
#
# Build PocketX Writer and flash it into ONE OTA slot.
#
#   ./scripts/flash-app.sh --slot 1 [--port /dev/cu.usbmodemXXXX] [--no-select]
#
# Writes only the application binary at the slot offset. The bootloader and the
# partition table are never touched, so the other OTA slot -- and whatever
# firmware lives there -- is left intact. This is deliberately NOT
# `pio run -t upload`, which would flash a partition table too.
#
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/device-common.sh"

SLOT=""; PORT=""; SELECT=1
while [ $# -gt 0 ]; do
  case "$1" in
    --slot)      SLOT="$2"; shift 2 ;;
    --port)      PORT="$2"; shift 2 ;;
    --no-select) SELECT=0; shift ;;
    *) die "unknown argument: $1" ;;
  esac
done
[ "$SLOT" = "0" ] || [ "$SLOT" = "1" ] || die "usage: $0 --slot 0|1 [--port PORT] [--no-select]"

case "$SLOT" in
  0) OFFSET=0x10000  ;;
  1) OFFSET=0x7f0000 ;;
esac
SLOT_SIZE=$((0x7e0000))

info "building"
"$REPO_ROOT/.venv/bin/pio" run -e x4pro >/dev/null

FW="$REPO_ROOT/.pio/build/x4pro/firmware.bin"
[ -f "$FW" ] || die "build produced no firmware.bin"
SIZE=$(stat -f%z "$FW")
[ "$SIZE" -le "$SLOT_SIZE" ] \
  || die "firmware is $SIZE bytes, larger than the $SLOT_SIZE-byte slot"
info "firmware: $SIZE bytes ($(( SIZE * 100 / SLOT_SIZE ))% of slot app$SLOT)"

[ -n "$PORT" ] || PORT="$(detect_port)"

info "writing app$SLOT at $OFFSET (bootloader + partition table untouched)"
"$ESPTOOL" --port "$PORT" --baud 921600 write-flash \
  --flash-mode keep --flash-freq keep --flash-size keep \
  "$OFFSET" "$FW"

info "verifying"
"$ESPTOOL" --port "$PORT" --baud 921600 verify-flash \
  --flash-mode keep --flash-freq keep --flash-size keep \
  "$OFFSET" "$FW"

if [ "$SELECT" -eq 1 ]; then
  info "selecting app$SLOT as boot slot"
  # The port drops while the device reboots after the write; wait for it back.
  for _ in $(seq 1 30); do [ -e "$PORT" ] && break; sleep 1; done
  "$REPO_ROOT/.venv/bin/python" "$REPO_ROOT/scripts/select-slot.py" --port "$PORT" --slot "$SLOT"
else
  info "left the boot slot unchanged (--no-select)"
fi

info "done"
