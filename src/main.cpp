// PocketX Writer — M4: BLE keyboard end to end.
//
// Pairs with a BLE HID keyboard, translates HID usages through the German T1
// layout (src/core/de_keymap.*, covered by native unit tests) and echoes the
// text over serial. On screen each character appears as a block in a grid --
// enough to see and feel the input path. Real glyph rendering needs a font
// pipeline and belongs to M5.
//
// Screen updates follow the M3 result: never block on the panel, coalesce
// keystrokes and refresh only when it is free.
//
// Hold RIGHT for 2 s to boot the other OTA slot.

#include <Arduino.h>
#include <esp_ota_ops.h>

#include <BleKeyboardHost.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <XteinkDetect.h>

#include "core/de_keymap.h"

namespace {

const auto& kPins = BoardConfig::ACTIVE.display;
EInkDisplay display(kPins.sclk, kPins.mosi, kPins.cs, kPins.dc, kPins.rst, kPins.busy);

constexpr int kSwitchButton = 7;
constexpr uint32_t kHoldMs = 2000;

constexpr uint16_t kW = 800, kH = 480;
constexpr uint16_t kRowBytes = kW / 8;

// Typed-character grid: one block per character.
constexpr uint16_t kCell = 16, kGap = 4, kMargin = 24, kTopBar = 64;
constexpr uint16_t kCols = (kW - 2 * kMargin) / (kCell + kGap);

pocketx::Dead gDead = pocketx::Dead::None;
uint16_t gTyped = 0;
uint32_t gPendingSince = 0;
bool gDirty = false;
bool gScanRequested = false;
uint32_t gConnectStartedAt = 0;

char gLine[512];
uint16_t gLineLen = 0;

void fillRect(uint8_t* fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
  for (uint16_t yy = y; yy < y + h && yy < kH; ++yy) {
    for (uint16_t xx = x; xx < x + w && xx < kW; ++xx) {
      const uint32_t idx = (uint32_t)yy * kRowBytes + (xx >> 3);
      const uint8_t bit = 0x80 >> (xx & 7);
      if (black) fb[idx] &= ~bit;   // 0 = black
      else fb[idx] |= bit;
    }
  }
}

void redraw(uint8_t* fb, bool connected) {
  memset(fb, 0xFF, (uint32_t)kRowBytes * kH);
  // Status bar: solid when connected, dashed while searching.
  if (connected) {
    fillRect(fb, kMargin, 24, kW - 2 * kMargin, 12, true);
  } else {
    for (uint16_t x = kMargin; x < kW - kMargin; x += 40)
      fillRect(fb, x, 24, 20, 12, true);
  }
  for (uint16_t i = 0; i < gTyped && i < kCols * 20; ++i) {
    const uint16_t c = i % kCols, r = i / kCols;
    fillRect(fb, kMargin + c * (kCell + kGap), kTopBar + r * (kCell + kGap), kCell, kCell, true);
  }
}

void flushLine() {
  if (!gLineLen) return;
  gLine[gLineLen] = 0;
  Serial.printf("[text] %s\n", gLine);
  gLineLen = 0;
}

void appendText(const char* s, uint8_t len) {
  for (uint8_t i = 0; i < len && gLineLen < sizeof(gLine) - 1; ++i) gLine[gLineLen++] = s[i];
}

void bootOtherSlot() {
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!other) return;
  Serial.printf("[..] switching to %s\n", other->label);
  if (esp_ota_set_boot_partition(other) != ESP_OK) return;
  Serial.flush();
  delay(100);
  esp_restart();
}

void pollSwitchButton() {
  static uint32_t downSince = 0;
  if (digitalRead(kSwitchButton) != LOW) { downSince = 0; return; }
  const uint32_t now = millis();
  if (downSince == 0) downSince = now;
  else if (now - downSince >= kHoldMs) { downSince = 0; bootOtherSlot(); }
}

void listDevices() {
  auto& ble = freeink::BleKeyboardHost::getInstance();
  const uint8_t n = ble.deviceCount();
  Serial.printf("[ble] %u device(s) seen:\n", n);
  for (uint8_t i = 0; i < n; ++i) {
    const auto& d = ble.device(i);
    Serial.printf("  %2u %-24s %-18s rssi=%4d %s%s\n", i, d.name, d.addr, d.rssi,
                  d.hid ? "HID " : "    ", d.connectable ? "" : "(not connectable)");
  }
  Serial.println("[ble] connect with:  c<index>   e.g. c0");
}

void handleScanResults() {
  auto& ble = freeink::BleKeyboardHost::getInstance();
  if (!gScanRequested || ble.isScanning()) return;
  gScanRequested = false;
  Serial.println("[ble] scan finished");
  listDevices();
}

// Deliberately NOT automatic. An earlier revision connected to the strongest
// HID advertiser in range and reached for a neighbour's TV remote: "the only
// HID device nearby" is not the same as "the user's keyboard". Pairing is a
// decision, so a human makes it.
void pollSerialCommands() {
  static char line[16];
  static uint8_t len = 0;
  auto& ble = freeink::BleKeyboardHost::getInstance();

  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;
    }
    line[len] = 0;
    const uint8_t n = len;
    len = 0;
    if (n == 0) continue;

    switch (line[0]) {
      case 's':
        Serial.println("[ble] scanning 8s...");
        ble.startScan(8000);
        gScanRequested = true;
        break;
      case 'l':
        listDevices();
        break;
      case 'c': {
        const int idx = atoi(line + 1);
        if (idx < 0 || idx >= ble.deviceCount()) { Serial.println("[ble] bad index"); break; }
        const auto& d = ble.device((uint8_t)idx);
        Serial.printf("[ble] connecting to %s (%s)\n", d.name, d.addr);
        gConnectStartedAt = millis();
        ble.connect(d.addr);
        break;
      }
      case 'p':
        Serial.printf("[ble] %u bonded:\n", ble.pairedCount());
        for (uint8_t i = 0; i < ble.pairedCount(); ++i)
          Serial.printf("  %s (%s)\n", ble.paired(i).name, ble.paired(i).addr);
        break;
      case 'f':
        while (ble.pairedCount()) {
          Serial.printf("[ble] forgetting %s\n", ble.paired(0).addr);
          ble.forget(ble.paired(0).addr);
        }
        break;
      case 'd':
        ble.disconnect();
        Serial.println("[ble] disconnected");
        break;
      default:
        Serial.println("[ble] s=scan  l=list  c<n>=connect  p=pairings  f=forget all  d=disconnect");
    }
  }
}

// A connect attempt that never completes would otherwise wedge the UI silently.
void pollConnectTimeout() {
  auto& ble = freeink::BleKeyboardHost::getInstance();
  char why[48];
  if (ble.takeConnectFailure(why, sizeof(why)))
    Serial.printf("[ble] connect failed: %s\n", why);

  if (!gConnectStartedAt) return;
  if (ble.isConnected() || !ble.isConnecting()) { gConnectStartedAt = 0; return; }
  if (millis() - gConnectStartedAt > 15000) {
    gConnectStartedAt = 0;
    ble.disconnect();
    Serial.println("[ble] connect timed out after 15s — aborted");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  esp_ota_mark_app_valid_cancel_rollback();
  pinMode(kSwitchButton, INPUT_PULLUP);

  freeink::applyXteinkDisplayController();
  display.begin();
  redraw(display.getFrameBuffer(), false);
  display.displayBuffer(EInkDisplay::FULL_REFRESH);

  Serial.println("\n=== PocketX Writer :: M4 BLE keyboard ===");
  auto& ble = freeink::BleKeyboardHost::getInstance();
  if (!ble.begin("PocketX Writer")) {
    Serial.println("[err] BLE init failed");
    return;
  }
  Serial.printf("[ble] host up, %u known pairing(s)\n", ble.pairedCount());
  for (uint8_t i = 0; i < ble.pairedCount(); ++i)
    Serial.printf("  bonded: %s (%s)\n", ble.paired(i).name, ble.paired(i).addr);

  if (ble.pairedCount() == 0) {
    Serial.println("[ble] scanning 8s — put your keyboard in pairing mode");
    Serial.println("[ble] commands: s=scan  l=list  c<n>=connect  p=pairings  f=forget all");
    ble.startScan(8000);
    gScanRequested = true;
  } else {
    Serial.println("[ble] waiting for auto-reconnect to a bonded keyboard");
  }
}

void loop() {
  auto& ble = freeink::BleKeyboardHost::getInstance();
  ble.poll();
  pollSwitchButton();
  handleScanResults();
  pollSerialCommands();
  pollConnectTimeout();

  static bool wasConnected = false;
  const bool connected = ble.isConnected();
  if (connected != wasConnected) {
    wasConnected = connected;
    Serial.printf("[ble] %s%s%s\n", connected ? "connected to " : "disconnected",
                  connected ? ble.connectedName() : "", connected ? "" : "");
    if (connected) ble.releaseScanResults();
    gDirty = true;
    gPendingSince = millis();
  }

  freeink::KeyEvent ev;
  while (ble.popKey(ev)) {
    if (ev.special == freeink::SpecialKey::Enter) {
      flushLine();
      Serial.println("[key] Enter");
      continue;
    }
    if (ev.special == freeink::SpecialKey::Backspace) {
      if (gLineLen) --gLineLen;
      if (gTyped) --gTyped;
      Serial.println("[key] Backspace");
      gDirty = true;
      if (!gPendingSince) gPendingSince = millis();
      continue;
    }
    if (ev.special != freeink::SpecialKey::None) {
      Serial.printf("[key] special=%u\n", (unsigned)ev.special);
      continue;
    }

    pocketx::KeyText txt;
    if (!pocketx::deTranslate(ev.keycode, ev.mods, gDead, txt)) {
      Serial.printf("[key] ignored usage=0x%02X mods=0x%02X\n", ev.keycode, ev.mods);
      continue;
    }
    if (txt.consumedAsDead) { Serial.println("[key] dead key armed"); continue; }

    appendText(txt.utf8, txt.len);
    ++gTyped;
    gDirty = true;
    if (!gPendingSince) gPendingSince = millis();
    // Show what the SDK's US table would have produced, so a layout regression
    // is visible rather than silent.
    Serial.printf("[key] usage=0x%02X mods=0x%02X -> \"%s\"  (sdk ascii: '%c')\n",
                  ev.keycode, ev.mods, txt.utf8, ev.ch ? ev.ch : ' ');
  }

  // M3's lesson: never block on the panel. Coalesce and refresh when it is free.
  if (gDirty && !display.refreshBusy() && millis() - gPendingSince >= 120) {
    redraw(display.getFrameBuffer(), connected);
    display.displayBufferAsync(EInkDisplay::FAST_REFRESH);
    gDirty = false;
    gPendingSince = 0;
  }

  delay(5);
}
