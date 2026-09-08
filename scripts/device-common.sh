#!/usr/bin/env bash
# Shared helpers for the device scripts.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESPTOOL="${ESPTOOL:-$REPO_ROOT/.venv/bin/esptool}"
FLASH_SIZE_BYTES=$((16 * 1024 * 1024))   # 0x1000000

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarn:\033[0m %s\n' "$*" >&2; }

[ -x "$ESPTOOL" ] || die "esptool not found at $ESPTOOL (run: ./.venv/bin/pip install esptool)"

# Pick the ESP32 USB CDC port. Excludes the Mac's built-in Bluetooth/debug ports.
detect_port() {
  local candidates
  candidates=$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null || true)
  local count
  count=$(printf '%s\n' "$candidates" | grep -c . || true)
  [ "$count" -eq 0 ] && die "no USB serial device found — is the X4 Pro plugged in?"
  if [ "$count" -gt 1 ]; then
    warn "multiple serial devices found:"
    printf '%s\n' "$candidates" >&2
    die "pass the right one explicitly: $0 --port /dev/cu.usbmodemXXXX"
  fi
  printf '%s\n' "$candidates" | head -1
}

# The device MAC is our device identity. Restoring one unit's dump onto another
# would write the wrong per-device calibration (the OEM hw_calib NVS namespace
# names the panel controller), so every restore is gated on it matching.
read_mac() {
  local port="$1"
  "$ESPTOOL" --port "$port" read-mac 2>/dev/null \
    | grep -iE '^MAC:' | head -1 | awk '{print $2}'
}

sha256_of() {
  shasum -a 256 "$1" | awk '{print $1}'
}
