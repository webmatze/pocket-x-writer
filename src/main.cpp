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
#include "core/text_render.h"
#include "fonts/NotoSans261bpp.h"

namespace {

const auto& kPins = BoardConfig::ACTIVE.display;
EInkDisplay display(kPins.sclk, kPins.mosi, kPins.cs, kPins.dc, kPins.rst, kPins.busy);

constexpr int kSwitchButton = 7;
constexpr uint32_t kHoldMs = 2000;

// Page layout. 800x480 with a 26 px font (yAdvance 36) leaves 12 body lines
// under a status strip.
constexpr uint16_t kW = 800, kH = 480;
constexpr uint16_t kRowBytes = kW / 8;
constexpr int32_t kMargin = 24;
constexpr int32_t kTextTop = 56;
constexpr uint32_t kTextWidth = kW - 2 * kMargin;
constexpr uint16_t kMaxLines = 32;

const pocketx::Font& kFont = freeink::ui::kNotoSans261bppFont;

pocketx::Dead gDead = pocketx::Dead::None;
uint32_t gPendingSince = 0;
bool gDirty = false;
bool gScanRequested = false;
uint32_t gConnectStartedAt = 0;
// Hunt mode: keep scanning and report devices the moment they appear. A
// keyboard advertises in bursts and can be quiet during any single short scan.
bool gHunting = false;
uint32_t gHuntUntil = 0;
uint8_t gReported = 0;

// The document. A flat UTF-8 buffer is enough to prove the pipeline; the real
// editor core (gap buffer, undo, cursor) is the next step.
char gText[8192];
uint32_t gTextLen = 0;

void appendText(const char* s, uint8_t len) {
  for (uint8_t i = 0; i < len && gTextLen < sizeof(gText) - 1; ++i) gText[gTextLen++] = s[i];
  gText[gTextLen] = 0;
}

// Delete one CHARACTER, not one byte: chopping a byte off "ä" would leave a
// broken UTF-8 sequence in the document.
void backspaceChar() {
  if (!gTextLen) return;
  uint32_t i = gTextLen - 1;
  while (i > 0 && (gText[i] & 0xC0) == 0x80) --i;   // step back over continuation bytes
  gTextLen = i;
  gText[gTextLen] = 0;
}

void redraw(uint8_t* fb, bool connected, const char* status) {
  const pocketx::Canvas canvas{fb, kW, kH, kRowBytes};
  memset(fb, 0xFF, (uint32_t)kRowBytes * kH);

  // Status strip.
  pocketx::drawText(canvas, kFont, kMargin, 34, status);
  pocketx::fillRect(canvas, pocketx::Rect{kMargin, 44, (int32_t)kTextWidth, 1}, true);

  pocketx::Line lines[kMaxLines];
  const uint16_t n = pocketx::wrapText(kFont, gText, kTextWidth, lines, kMaxLines);

  // Show the tail of the document: a writer wants to see what they just typed.
  const uint16_t fits = (kH - kTextTop) / kFont.yAdvance;
  const uint16_t firstLine = n > fits ? n - fits : 0;

  int32_t y = kTextTop + kFont.ascent;
  char buf[256];
  for (uint16_t i = firstLine; i < n; ++i) {
    uint32_t len = lines[i].end - lines[i].begin;
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, gText + lines[i].begin, len);
    buf[len] = 0;
    pocketx::drawText(canvas, kFont, kMargin, y, buf);
    y += kFont.yAdvance;
  }

  // Caret: a filled block after the last line, so the page looks alive even
  // between refreshes.
  if (n) {
    uint32_t len = lines[n - 1].end - lines[n - 1].begin;
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, gText + lines[n - 1].begin, len);
    buf[len] = 0;
    const int32_t caretX = kMargin + (int32_t)pocketx::measureText(kFont, buf);
    const int32_t caretY = kTextTop + (int32_t)(n - 1 - firstLine) * kFont.yAdvance;
    pocketx::fillRect(canvas, pocketx::Rect{caretX + 2, caretY + 4, 2, kFont.ascent}, true);
  }
  (void)connected;
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
  Serial.printf("[ble] %u device(s) seen, strongest first:\n", n);
  // Sort by RSSI. A keyboard on the desk is 40 dB louder than the neighbours,
  // so the one you want is at the top of the list.
  uint8_t order[freeink::BleKeyboardHost::kMaxDiscovered];
  for (uint8_t i = 0; i < n; ++i) order[i] = i;
  for (uint8_t i = 1; i < n; ++i)
    for (uint8_t j = i; j > 0 && ble.device(order[j]).rssi > ble.device(order[j - 1]).rssi; --j) {
      const uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }
  for (uint8_t k = 0; k < n; ++k) {
    const uint8_t i = order[k];
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

void pollHunt() {
  if (!gHunting) return;
  auto& ble = freeink::BleKeyboardHost::getInstance();

  // Report anything new the instant it lands, so a burst-advertising keyboard
  // is visible even if it goes quiet again a second later.
  const uint8_t n = ble.deviceCount();
  for (uint8_t i = gReported; i < n; ++i) {
    const auto& d = ble.device(i);
    const bool interesting = d.hid || d.rssi > -65;
    Serial.printf("%s %2u %-24s %-18s rssi=%4d %s%s\n", interesting ? "  >>>" : "     ", i, d.name, d.addr,
                  d.rssi, d.hid ? "HID " : "", d.connectable ? "" : "(not connectable)");
    if (interesting)
      Serial.printf("      ^ candidate — connect with: c%u\n", i);
  }
  gReported = n;

  if (!ble.isScanning() && (int32_t)(gHuntUntil - millis()) > 0) ble.startScan(15000);
  if ((int32_t)(gHuntUntil - millis()) <= 0) {
    gHunting = false;
    ble.stopScan();
    Serial.println("[ble] hunt over");
    listDevices();
  }
}

// Deliberately NOT automatic. An earlier revision connected to the strongest
// HID advertiser in range and reached for a neighbour's TV remote: "the only
// HID device nearby" is not the same as "the user's keyboard". Pairing is a
// decision, so a human makes it.
void pollSerialCommands() {
  // Must hold the longest command: 'a' + "AA:BB:CC:DD:EE:FF" + NUL = 19.
  static char line[40];
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
      case 'h':
        gHunting = true;
        gHuntUntil = millis() + 90000;
        gReported = 0;
        Serial.println("[ble] HUNT: scanning continuously for 90s.");
        Serial.println("[ble] Put the keyboard into pairing mode NOW — new devices print as they appear.");
        break;
      case 'a': {
        // Connect straight to an address, e.g. from macOS's Bluetooth panel.
        // connect() tries both address types when it has not seen the device.
        const char* addr = line + 1;
        while (*addr == ' ') ++addr;
        if (strlen(addr) != 17) { Serial.println("[ble] expected: a AA:BB:CC:DD:EE:FF"); break; }
        Serial.printf("[ble] connecting directly to %s\n", addr);
        gConnectStartedAt = millis();
        ble.connect(addr);
        break;
      }
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
        Serial.println("[ble] s=scan  h=hunt(90s)  l=list  c<n>=connect  a<addr>=connect by address");
        Serial.println("[ble] p=pairings  f=forget all  d=disconnect");
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
  redraw(display.getFrameBuffer(), false, "PocketX Writer  Tastatur suchen...");
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
  pollHunt();
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
    // The Keychron K2 HE puts a constant 0x39 (Caps Lock) in byte 2 of every
    // report, including the all-released one, so the SDK's slot decoder reads it
    // as a permanently held key and keeps re-emitting it. It is a quirk of this
    // keyboard's report format, not a real keypress: Caps Lock is demonstrably
    // NOT engaged, since Shift+h still yields "H" rather than "h". Drop it before
    // it reaches the editor.
    if (ev.keycode == 0x39) continue;
    if (ev.special == freeink::SpecialKey::Enter) {
      appendText("\n", 1);
      gDirty = true;
      if (!gPendingSince) gPendingSince = millis();
      continue;
    }
    if (ev.special == freeink::SpecialKey::Backspace) {
      backspaceChar();
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
    gDirty = true;
    if (!gPendingSince) gPendingSince = millis();
    // Show what the SDK's US table would have produced, so a layout regression
    // is visible rather than silent.
    Serial.printf("[key] 0x%02X/%02X -> \"%s\" (us:'%c')  doc=%lub\n", ev.keycode, ev.mods, txt.utf8,
                  ev.ch ? ev.ch : ' ', (unsigned long)gTextLen);
  }

  // M3's lesson: never block on the panel. Coalesce and refresh when it is free.
  if (gDirty && !display.refreshBusy() && millis() - gPendingSince >= 120) {
    char status[64];
    snprintf(status, sizeof(status), "PocketX Writer  %s  %lu Zeichen",
             connected ? ble.connectedName() : "keine Tastatur", (unsigned long)gTextLen);
    redraw(display.getFrameBuffer(), connected, status);
    display.displayBufferAsync(EInkDisplay::FAST_REFRESH);
    gDirty = false;
    gPendingSince = 0;
  }

  delay(5);
}
