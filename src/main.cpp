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
#include <SDCardManager.h>
#include <UsbMassStorage.h>
#include <EInkDisplay.h>
#include <XteinkDetect.h>

#include "core/de_keymap.h"
#include "storage.h"

#include "core/document.h"
#include "core/project.h"
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

// The document lives in PSRAM -- 8 MB of it is exactly what makes this device
// suited to writing, and a chapter never comes close to the limit.
constexpr uint32_t kDocCapacity = 256 * 1024;
constexpr uint32_t kUndoArena = 16 * 1024;
constexpr uint16_t kUndoRecords = 512;

pocketx::Document* gDoc = nullptr;
pocketx::Storage gStorage;

constexpr const char* kBookTitle = "Mein Buch";
// A daily word goal, the number a writer actually steers by.
constexpr uint32_t kDailyGoal = 1000;

pocketx::Chapter gChapters[pocketx::Storage::kMaxChapters];
uint16_t gChapterCount = 0;
uint16_t gChapterIndex = 0;
// Words already written when the chapter was opened, so the goal measures
// today's output rather than the chapter's total length.
uint32_t gWordsAtOpen = 0;

// USB transfer mode. Once the filesystem is handed to the host, this firmware
// must not touch the card again -- the SDK's contract is explicit that the owner
// suspends all filesystem use before begin() and reboots afterwards.
freeink::UsbMassStorage gMsc;
bool gUsbMode = false;

// --- ghost management ------------------------------------------------------
// FAST is the UC8279's DU partial: it transitions only changed pixels and
// accumulates residual charge, which is what leaves shadows of earlier text and
// why fresh letters start faint and darken over later passes. HALF is the
// driver's charge SCRUB -- it seeds the old plane as the complement of the
// target so EVERY pixel makes a transition, clearing that residue. It costs more
// time, so it is spent where the writer will not feel it.
// How urgently the page needs scrubbing depends on how much AREA was repainted
// with partials, not how many partials there were. Typing a character and
// clearing a full-page selection both count as one refresh, yet the second
// leaves roughly as much residue as a hundred keystrokes -- which is why a fixed
// pause felt far too long after selecting all and deselecting again.
//
// gResidue accumulates repainted area in percent of a full page. The pause
// before scrubbing shrinks as it grows, so a heavy change is cleaned up almost
// at once while ordinary typing is left alone.
uint32_t gResidue = 0;
constexpr uint32_t kResidueForceScrub = 300;   // three pages' worth: clean up regardless
constexpr uint32_t kIdleScrubMsLow = 6000;     // after light typing
constexpr uint32_t kIdleScrubMsMid = 2500;
constexpr uint32_t kIdleScrubMsHigh = 1000;    // after a page-sized repaint

uint32_t idleScrubDelay() {
  if (gResidue >= 80) return kIdleScrubMsHigh;
  if (gResidue >= 25) return kIdleScrubMsMid;
  return kIdleScrubMsLow;
}
// Below this fraction of the page, refresh just the changed rectangle. Above it
// the window costs the same as a full pass, so there is nothing to gain.
constexpr uint32_t kWindowMaxRows = 200;
// Previous frame, kept to diff against. Trusting a frame comparison rather than
// hand-reported damage means a missed report cannot leave stale pixels behind.
uint8_t* gPrevFrame = nullptr;

// The status line sits at the top of the page. If it changes while the writer is
// typing at the bottom, the single damage rectangle spans almost the whole page
// and the cheap windowed refresh is lost. It carries nothing that needs
// per-keystroke precision, so it is rebuilt only when the writer pauses or the
// document is saved -- which keeps typing damage confined to one text line.
char gStatusText[128] = "PocketX Writer";
bool gStatusStale = true;
uint32_t gLastRefreshAt = 0;
bool gForceFullRefresh = false;

// Clipboard in PSRAM. Sized to hold a long passage, not just a word -- moving a
// paragraph between chapters is the operation this is really for.
constexpr uint32_t kClipboardSize = 16 * 1024;
char* gClipboard = nullptr;
uint32_t gClipboardLen = 0;
// Idle before an automatic save. Long enough not to save mid-word, short enough
// that a flat battery costs a sentence rather than a session.
constexpr uint32_t kAutosaveIdleMs = 8000;
uint32_t gLastEditAt = 0;
uint32_t gLastSaveAt = 0;
bool gSaveFailed = false;
uint16_t gScrollLine = 0;   // first wrapped line drawn

// Where the cursor sits in the wrapped layout, recomputed on each redraw.
uint16_t gCursorLine = 0;
int32_t gCursorX = 0;

// Lay out the document, work out where the cursor is, scroll so it stays
// visible, and draw. Returns nothing: the caller decides how to push the frame.
void redraw(uint8_t* fb, const char* status) {
  const pocketx::Canvas canvas{fb, kW, kH, kRowBytes};
  memset(fb, 0xFF, (uint32_t)kRowBytes * kH);

  pocketx::drawText(canvas, kFont, kMargin, 34, status);
  pocketx::fillRect(canvas, pocketx::Rect{kMargin, 44, (int32_t)kTextWidth, 1}, true);

  const char* text = gDoc->text();
  static pocketx::Line lines[kMaxLines];
  uint16_t n = pocketx::wrapText(kFont, text, kTextWidth, lines, kMaxLines);
  if (!n) { lines[0] = pocketx::Line{0, 0}; n = 1; }

  // Locate the cursor in the wrapped layout. The cursor belongs to the last
  // line whose start is at or before it, so a cursor sitting exactly on a line
  // break lands at the start of the new line rather than trailing the old one.
  const uint32_t cur = gDoc->cursor();
  gCursorLine = 0;
  for (uint16_t i = 0; i < n; ++i)
    if (lines[i].begin <= cur) gCursorLine = i;

  char buf[512];
  {
    uint32_t len = cur - lines[gCursorLine].begin;
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, text + lines[gCursorLine].begin, len);
    buf[len] = 0;
    gCursorX = kMargin + (int32_t)pocketx::measureText(kFont, buf);
  }

  const uint16_t fits = (kH - kTextTop) / kFont.yAdvance;
  // Keep the cursor on screen without jumping the page around more than needed.
  if (gCursorLine < gScrollLine) gScrollLine = gCursorLine;
  if (gCursorLine >= gScrollLine + fits) gScrollLine = gCursorLine - fits + 1;
  if (gScrollLine > n) gScrollLine = n ? n - 1 : 0;

  const uint32_t selFrom = gDoc->selectionBegin();
  const uint32_t selTo = gDoc->selectionEnd();
  const bool hasSel = gDoc->hasSelection();

  int32_t y = kTextTop + kFont.ascent;
  for (uint16_t i = gScrollLine; i < n && i < gScrollLine + fits; ++i) {
    uint32_t len = lines[i].end - lines[i].begin;
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, text + lines[i].begin, len);
    buf[len] = 0;
    pocketx::drawText(canvas, kFont, kMargin, y, buf);

    // Invert the part of this line that falls inside the selection. Drawing the
    // text first and flipping afterwards avoids a white-ink draw path, and on a
    // 1-bit panel white-on-black is unmistakable even against ghost residue.
    if (hasSel && selTo > lines[i].begin && selFrom < lines[i].end) {
      const uint32_t a = selFrom > lines[i].begin ? selFrom : lines[i].begin;
      const uint32_t b = selTo < lines[i].end ? selTo : lines[i].end;
      char head[512];
      uint32_t hn = a - lines[i].begin;
      if (hn > sizeof(head) - 1) hn = sizeof(head) - 1;
      memcpy(head, text + lines[i].begin, hn); head[hn] = 0;
      const int32_t x0 = kMargin + (int32_t)pocketx::measureText(kFont, head);
      uint32_t mn = b - a;
      if (mn > sizeof(head) - 1) mn = sizeof(head) - 1;
      memcpy(head, text + a, mn); head[mn] = 0;
      int32_t w = (int32_t)pocketx::measureText(kFont, head);
      // A selected newline shows as a thin bar, so an empty line still reads
      // as part of the selection rather than vanishing from it.
      if (w == 0) w = 6;
      pocketx::invertRect(canvas, pocketx::Rect{x0, y - kFont.ascent, w, kFont.yAdvance - 2});
    }
    y += kFont.yAdvance;
  }

  // Caret. A 2 px hairline was hard to find on this panel, and ghosting left
  // several of them visible at once. A solid block with a baseline foot reads as
  // one unmistakable mark even against residue.
  if (!hasSel) {
    const int32_t caretY = kTextTop + (int32_t)(gCursorLine - gScrollLine) * kFont.yAdvance;
    pocketx::fillRect(canvas, pocketx::Rect{gCursorX + 1, caretY + 3, 5, kFont.ascent - 1}, true);
    pocketx::fillRect(canvas, pocketx::Rect{gCursorX - 2, caretY + kFont.ascent + 2, 11, 3}, true);
  }
}

// Vertical movement is a view operation: it means "same x, one line up/down" in
// the wrapped layout, which only the layout knows.
void moveCursorVertically(int dir, bool extend) {
  const char* text = gDoc->text();
  static pocketx::Line lines[kMaxLines];
  const uint16_t n = pocketx::wrapText(kFont, text, kTextWidth, lines, kMaxLines);
  if (!n) return;

  const uint32_t cur = gDoc->cursor();
  uint16_t line = 0;
  for (uint16_t i = 0; i < n; ++i)
    if (lines[i].begin <= cur) line = i;

  const int32_t target = dir < 0 ? (int32_t)line - 1 : (int32_t)line + 1;
  if (target < 0 || target >= n) {
    // No line to move to -- but the keypress must still resolve the selection.
    // Returning early here is why Ctrl+A followed by Down left the whole page
    // highlighted with no way out: Up happened to work only because it reached
    // setCursor, which collapses as a side effect.
    if (!extend && gDoc->hasSelection())
      gDoc->setCursor(dir > 0 ? gDoc->selectionEnd() : gDoc->selectionBegin());
    return;
  }

  // Preserve the visual column: walk the target line until the pen passes the
  // cursor's x. Character widths differ, so this is a search, not arithmetic.
  char buf[512];
  uint32_t len = lines[gCursorLine].end - lines[gCursorLine].begin;
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  memcpy(buf, text + lines[gCursorLine].begin, cur - lines[gCursorLine].begin);
  buf[cur - lines[gCursorLine].begin] = 0;
  const uint32_t wantX = pocketx::measureText(kFont, buf);

  const pocketx::Line& tl = lines[target];
  uint32_t best = tl.begin, x = 0;
  for (uint32_t i = tl.begin; i < tl.end;) {
    uint32_t cp = 0;
    const uint8_t adv = pocketx::utf8Next(text + i, &cp);
    const uint32_t w = pocketx::glyphAdvance(kFont, cp);
    if (x + w / 2 > wantX) break;
    x += w;
    i += adv;
    best = i;
  }
  gDoc->setCursor(best, extend);
}

bool saveCurrentChapter() {
  if (!gChapterCount) return false;
  if (!gStorage.saveChapter(*gDoc, gChapters[gChapterIndex])) { gSaveFailed = true; return false; }
  gDoc->markClean();
  gDoc->breakUndoGroup();     // a save is a natural undo boundary
  gLastSaveAt = millis();
  gSaveFailed = false;
  gStatusStale = true;
  return true;
}

void openChapter(uint16_t index) {
  if (index >= gChapterCount) return;
  // Never switch away from unsaved work.
  if (gDoc->dirty()) saveCurrentChapter();
  gChapterIndex = index;
  gStorage.loadChapter(*gDoc, gChapters[index]);
  gWordsAtOpen = pocketx::countWords(gDoc->text());
  gScrollLine = 0;
  // A whole new page of text: without a full flash the previous chapter stays
  // legible underneath it, which is exactly what the ghosting looked like.
  gForceFullRefresh = true;
  gStatusStale = true;
  if (gPrevFrame) memset(gPrevFrame, 0, (uint32_t)kRowBytes * kH);   // force a full diff
  gDirty = true;
  if (!gPendingSince) gPendingSince = millis();
}

void newChapter() {
  if (gDoc->dirty()) saveCurrentChapter();
  pocketx::Chapter c;
  if (!gStorage.createChapter("", &c)) return;
  gChapterCount = gStorage.listChapters(gChapters, pocketx::Storage::kMaxChapters);
  for (uint16_t i = 0; i < gChapterCount; ++i)
    if (gChapters[i].number == c.number) { openChapter(i); return; }
}

// Hand the SD card to the Mac over the same USB-C cable that powers the device.
// Everything is drawn BEFORE the filesystem is detached, because afterwards the
// card belongs to the host and any access from here would race it.
void enterUsbTransfer() {
  if (gUsbMode) return;
  saveCurrentChapter();

  uint8_t* fb = display.getFrameBuffer();
  const pocketx::Canvas canvas{fb, kW, kH, kRowBytes};
  memset(fb, 0xFF, (uint32_t)kRowBytes * kH);
  pocketx::drawText(canvas, kFont, kMargin, 120, "USB-Transfer aktiv");
  pocketx::drawText(canvas, kFont, kMargin, 170, "Die SD-Karte liegt jetzt am Mac.");
  pocketx::drawText(canvas, kFont, kMargin, 210, "Nach dem Auswerfen startet das");
  pocketx::drawText(canvas, kFont, kMargin, 250, "Ger\xC3\xA4t neu.");
  display.displayBuffer(EInkDisplay::FULL_REFRESH);

  auto& sd = SDCardManager::getInstance();
  FsBlockDeviceInterface* dev = sd.detachFilesystemForRawAccess();
  if (!dev || !gMsc.begin(dev)) {
    Serial.println("[usb] could not start mass storage");
    pocketx::drawText(canvas, kFont, kMargin, 320, "Fehler: kein Zugriff auf die Karte");
    display.displayBuffer(EInkDisplay::FAST_REFRESH);
    return;
  }
  gUsbMode = true;
  Serial.println("[usb] mass storage active — waiting for the host");
}

// The card cannot be safely shared, so the way back is a reboot: it remounts the
// filesystem cleanly and reloads the chapter, including anything the Mac wrote.
void pollUsbTransfer() {
  if (!gUsbMode) return;
  const auto st = gMsc.state();
  if (st == freeink::UsbMassStorageState::Ejected ||
      st == freeink::UsbMassStorageState::Disconnected) {
    Serial.println("[usb] host finished — rebooting to remount");
    gMsc.end();
    Serial.flush();
    delay(200);
    esp_restart();
  }
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

  // Document storage in PSRAM: 8 MB is the reason this device suits writing.
  char* docBuf = (char*)heap_caps_malloc(kDocCapacity, MALLOC_CAP_SPIRAM);
  char* undoArena = (char*)heap_caps_malloc(kUndoArena, MALLOC_CAP_SPIRAM);
  auto* undoRecs = (pocketx::Document::Record*)heap_caps_malloc(
      sizeof(pocketx::Document::Record) * kUndoRecords, MALLOC_CAP_SPIRAM);
  static pocketx::Document doc(docBuf, docBuf ? kDocCapacity : 0,
                               [&] {
                                 pocketx::Document::UndoConfig u;
                                 u.arena = undoArena;
                                 u.arenaSize = undoArena ? kUndoArena : 0;
                                 u.records = undoRecs;
                                 u.maxRecords = undoRecs ? kUndoRecords : 0;
                                 return u;
                               }());
  gDoc = &doc;
  gClipboard = (char*)heap_caps_malloc(kClipboardSize, MALLOC_CAP_SPIRAM);
  gPrevFrame = (uint8_t*)heap_caps_malloc((uint32_t)kRowBytes * kH, MALLOC_CAP_SPIRAM);
  Serial.printf("[doc] %lu KB document + %lu KB undo in PSRAM (%s)\n",
                (unsigned long)(kDocCapacity / 1024), (unsigned long)(kUndoArena / 1024),
                docBuf ? "ok" : "ALLOCATION FAILED");

  if (gStorage.begin() && gStorage.openBook(kBookTitle)) {
    gChapterCount = gStorage.listChapters(gChapters, pocketx::Storage::kMaxChapters);
    if (gChapterCount == 0) {
      pocketx::Chapter c;
      if (gStorage.createChapter("", &c))
        gChapterCount = gStorage.listChapters(gChapters, pocketx::Storage::kMaxChapters);
    }
    if (gChapterCount) {
      // Resume where the writer left off: the last chapter is the one being
      // worked on far more often than the first.
      gChapterIndex = gChapterCount - 1;
      gStorage.loadChapter(doc, gChapters[gChapterIndex]);
      gWordsAtOpen = pocketx::countWords(doc.text());
    }
    Serial.printf("[book] %u chapter(s)\n", gChapterCount);
  }

  freeink::applyXteinkDisplayController();
  display.begin();
  redraw(display.getFrameBuffer(), "PocketX Writer  Tastatur suchen...");
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
  pollUsbTransfer();
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
    // report, so the slot decoder reads it as a permanently held key. It is a
    // quirk of this keyboard's report format, not a real keypress: Caps Lock is
    // demonstrably NOT engaged, since Shift+h still yields "H" rather than "h".
    if (ev.keycode == 0x39) continue;

    bool changed = false;
    // Command counts as Control for editor shortcuts, so the same keys work
    // whichever host mode the keyboard is switched to. deTranslate already
    // refuses GUI chords, so this cannot swallow text.
    const bool ctrl = ev.mods & (pocketx::kModLCtrl | pocketx::kModRCtrl |
                                 pocketx::kModLGui | pocketx::kModRGui);
    const bool shift = ev.mods & (pocketx::kModLShift | pocketx::kModRShift);

    // Ctrl+Z is undo. On a German keyboard the Z cap is HID usage 0x1C -- using
    // 0x1D here would bind the key labelled Y.
    // Ctrl+S saves immediately.
    if (gUsbMode) continue;                   // the card belongs to the host now

    if (ctrl && ev.keycode == 0x18) {          // Ctrl+U: USB transfer
      enterUsbTransfer();
      continue;
    }
    if (ctrl && ev.keycode == 0x16) {          // Ctrl+S
      saveCurrentChapter();
      changed = true;
    } else if (ctrl && ev.keycode == 0x06) {   // Ctrl+C: copy
      gClipboardLen = gDoc->copySelection(gClipboard, kClipboardSize);
      Serial.printf("[edit] copied %lu bytes\n", (unsigned long)gClipboardLen);
    } else if (ctrl && ev.keycode == 0x1B) {   // Ctrl+X: cut
      gClipboardLen = gDoc->copySelection(gClipboard, kClipboardSize);
      if (gClipboardLen) changed = gDoc->deleteSelection();
      Serial.printf("[edit] cut %lu bytes\n", (unsigned long)gClipboardLen);
    } else if (ctrl && ev.keycode == 0x19) {   // Ctrl+V: paste
      if (gClipboardLen) changed = gDoc->insert(gClipboard, gClipboardLen);
    } else if (ctrl && ev.keycode == 0x04) {   // Ctrl+A: select all
      gDoc->selectAll();
      changed = true;
    } else if (ctrl && ev.keycode == 0x11) {   // Ctrl+N: new chapter
      newChapter();
    } else if (ctrl && ev.keycode == 0x1C) {
      if (gDoc->undo()) changed = true;
      Serial.printf("[edit] undo -> %lu bytes\n", (unsigned long)gDoc->size());
    } else {
      switch (ev.special) {
        case freeink::SpecialKey::Enter:     changed = gDoc->insert("\n", 1); break;
        case freeink::SpecialKey::Backspace:
          changed = ctrl ? gDoc->deleteWordBefore() : gDoc->backspace();
          break;
        case freeink::SpecialKey::Delete:    changed = gDoc->deleteForward(); break;
        case freeink::SpecialKey::Left:
          ctrl ? gDoc->moveWordLeft(shift) : gDoc->moveLeft(shift);  changed = true; break;
        case freeink::SpecialKey::Right:
          ctrl ? gDoc->moveWordRight(shift) : gDoc->moveRight(shift); changed = true; break;
        case freeink::SpecialKey::Up:        moveCursorVertically(-1, shift); changed = true; break;
        case freeink::SpecialKey::Down:      moveCursorVertically(+1, shift); changed = true; break;
        case freeink::SpecialKey::Home:      gDoc->moveToLineStart(shift); changed = true; break;
        case freeink::SpecialKey::End:       gDoc->moveToLineEnd(shift);   changed = true; break;
        // Page keys move between chapters -- the navigation a book needs more
        // than paging within one chapter, which the arrows already cover.
        case freeink::SpecialKey::PageUp:
          if (gChapterIndex > 0) openChapter(gChapterIndex - 1);
          break;
        case freeink::SpecialKey::PageDown:
          if (gChapterIndex + 1 < gChapterCount) openChapter(gChapterIndex + 1);
          break;
        case freeink::SpecialKey::None: {
          if (ctrl) {
            // An unbound chord would otherwise disappear without trace, which is
            // exactly how the Cmd-vs-Ctrl mismatch stayed hidden.
            Serial.printf("[key] unbound chord: usage=0x%02X mods=0x%02X\n", ev.keycode, ev.mods);
            break;
          }
          pocketx::KeyText txt;
          if (!pocketx::deTranslate(ev.keycode, ev.mods, gDead, txt)) break;
          if (txt.consumedAsDead) break;          // dead key armed, nothing to insert yet
          changed = gDoc->insert(txt.utf8, txt.len);
          break;
        }
        default: break;
      }
    }

    if (changed) {
      gDirty = true;
      gLastEditAt = millis();
      if (!gPendingSince) gPendingSince = millis();
    }
  }

  // Autosave: only when the document actually changed and the writer has paused.
  // Saving mid-keystroke would stall input for no benefit.
  if (!gUsbMode && gDoc->dirty() && gLastEditAt && millis() - gLastEditAt >= kAutosaveIdleMs) {
    gLastEditAt = 0;
    saveCurrentChapter();
    gDirty = true;              // refresh so the status line reflects the result
    if (!gPendingSince) gPendingSince = millis();
  }

  // M3's lesson: never block on the panel. Coalesce and refresh when it is free.
  if (!gUsbMode && gDirty && !display.refreshBusy() && millis() - gPendingSince >= 120) {
    const bool idleNow = gLastEditAt == 0 || millis() - gLastEditAt >= idleScrubDelay();
    if (gStatusStale || idleNow) {
      const char* saveState = gSaveFailed          ? "NICHT GESPEICHERT"
                              : gDoc->dirty()      ? "*"
                              : gStorage.mounted() ? "gespeichert"
                                                   : "keine SD";
      const uint32_t words = pocketx::countWords(gDoc->text());
      const uint32_t today = words > gWordsAtOpen ? words - gWordsAtOpen : 0;
      snprintf(gStatusText, sizeof(gStatusText), "Kap. %u/%u  %lu Worte  Ziel %u%%  %s",
               gChapterCount ? gChapterIndex + 1 : 0, gChapterCount, (unsigned long)words,
               (unsigned)pocketx::goalPercent(today, kDailyGoal), saveState);
      gStatusStale = false;
    }
    const char* status = gStatusText;
    uint8_t* fb = display.getFrameBuffer();
    redraw(fb, status);

    // What actually changed on screen? Comparing frames is cheaper than a panel
    // refresh by orders of magnitude, and it catches the common case where a
    // keypress changed nothing visible at all.
    pocketx::Rect damage{0, 0, kW, kH};
    if (gPrevFrame) {
      const pocketx::Canvas now{fb, kW, kH, kRowBytes};
      const pocketx::Canvas prev{gPrevFrame, kW, kH, kRowBytes};
      damage = pocketx::diffCanvas(prev, now);
      if (damage.empty()) {          // nothing to show; do not burn a refresh
        gDirty = false;
        gPendingSince = 0;
        goto refresh_done;
      }
      memcpy(gPrevFrame, fb, (uint32_t)kRowBytes * kH);
    }

    {
      const uint32_t area = ((uint32_t)damage.w * (uint32_t)damage.h * 100u) /
                            ((uint32_t)kW * (uint32_t)kH);
      const bool idle = gLastEditAt == 0 || millis() - gLastEditAt >= idleScrubDelay();
      if (gForceFullRefresh) {
        display.displayBuffer(EInkDisplay::FULL_REFRESH);
        gForceFullRefresh = false;
        gResidue = 0;
      } else if (gResidue >= kResidueForceScrub || (idle && gResidue > 0)) {
        // The scrub has to cover the whole panel: residue sits wherever earlier
        // partials landed, not only where the last one did.
        display.displayBuffer(EInkDisplay::HALF_REFRESH);
        gResidue = 0;
      } else if ((uint32_t)damage.h <= kWindowMaxRows) {
        // Confine the partial to the rows that changed. It costs the same time
        // as a full pass on this panel, but only this band collects residue --
        // which is what makes the scrub rare enough to stop being a nuisance.
        display.displayWindow((uint16_t)damage.x, (uint16_t)damage.y, (uint16_t)damage.w,
                              (uint16_t)damage.h);
        gResidue += area ? area : 1;   // never zero, or tiny edits would never trigger a scrub
      } else {
        display.displayBufferAsync(EInkDisplay::FAST_REFRESH);
        gResidue += 100;               // a whole-panel partial dirties the whole panel
      }
    }

    gLastRefreshAt = millis();
    gDirty = false;
    gPendingSince = 0;
  refresh_done:;
  }

  // After a pause, clean the page once even when nothing else changed.
  if (!gUsbMode && !gDirty && gResidue > 0 && !display.refreshBusy() && gLastEditAt &&
      millis() - gLastEditAt >= idleScrubDelay()) {
    gResidue = 0;
    display.displayBuffer(EInkDisplay::HALF_REFRESH);
    gLastRefreshAt = millis();
  }

  delay(5);
}
