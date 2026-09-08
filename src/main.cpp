// PocketX Writer — M3 typing-latency spike.
//
// The one question that decides whether this product is worth building: how long
// after a keystroke does the character actually appear on the panel?
//
// This unit carries a UC8179, whose driver does NOT implement a windowed
// refresh -- displayWindow() falls through to a whole-panel Fast refresh (see
// docs/panel-uc8179.md). So every number here is a FULL 800x480 repaint. That is
// the honest baseline; a real window would only improve on it.
//
// Escape hatch preserved: hold RIGHT for 2 s to boot back into the other slot.

#include <Arduino.h>
#include <esp_ota_ops.h>

#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <XteinkDetect.h>

namespace {

const auto& kPins = BoardConfig::ACTIVE.display;
EInkDisplay display(kPins.sclk, kPins.mosi, kPins.cs, kPins.dc, kPins.rst,
                    kPins.busy);

constexpr int kSwitchButton = 7;   // Right nav, active-low (GPIO0 is a boot strap)
constexpr uint32_t kHoldMs = 2000;

constexpr uint16_t kW = 800, kH = 480;
constexpr uint16_t kRowBytes = kW / 8;      // 100
constexpr uint16_t kLineHeight = 26;        // plausible body-text leading
constexpr uint16_t kCharW = 12;             // plausible advance width

bool gPanelPromoted = false;

inline void setByte(uint8_t* fb, uint16_t x, uint16_t y, uint8_t v) {
  if (x >= kW || y >= kH) return;
  fb[(uint32_t)y * kRowBytes + (x / 8)] = v;
}

// Paint something that looks like a page of prose, so the refresh has realistic
// content to transition (an all-white panel is not a fair baseline: e-ink
// transition cost depends on how many pixels actually change).
void renderFakePage(uint8_t* fb, uint16_t lines) {
  memset(fb, 0xFF, (uint32_t)kRowBytes * kH);   // 0xFF = white
  uint32_t seed = 12345;
  for (uint16_t l = 0; l < lines; ++l) {
    const uint16_t yTop = 8 + l * kLineHeight;
    if (yTop + 16 >= kH) break;
    uint16_t x = 8;
    while (x < kW - 40) {
      seed = seed * 1103515245u + 12345u;
      const uint16_t wordLen = 2 + (seed >> 16) % 7;          // 2..8 chars
      for (uint16_t c = 0; c < wordLen && x < kW - 40; ++c) {
        for (uint16_t dy = 0; dy < 16; ++dy) {
          // crude glyph body: two dark bytes per character cell
          setByte(fb, x, yTop + dy, 0x00);
        }
        x += kCharW;
      }
      x += kCharW;                                            // word space
    }
  }
}

// Change exactly one character cell — the "keystroke".
void typeOneChar(uint8_t* fb, uint16_t index) {
  const uint16_t line = 6;                       // somewhere mid-page
  const uint16_t yTop = 8 + line * kLineHeight;
  const uint16_t x = 8 + (index % 50) * kCharW;
  for (uint16_t dy = 0; dy < 16; ++dy) setByte(fb, x, yTop + dy, 0x00);
}

struct Stats { uint32_t lo = UINT32_MAX, hi = 0; uint64_t sum = 0; uint16_t n = 0;
  void add(uint32_t v) { lo = min(lo, v); hi = max(hi, v); sum += v; ++n; }
  uint32_t avg() const { return n ? (uint32_t)(sum / n) : 0; } };

uint32_t timeBlocking(EInkDisplay::RefreshMode mode, uint8_t* fb, uint16_t seedLines) {
  renderFakePage(fb, seedLines);
  const uint32_t t0 = millis();
  display.displayBuffer(mode);
  return millis() - t0;
}

void runBenchmark() {
  uint8_t* fb = display.getFrameBuffer();
  if (!fb) { Serial.println("[err] no framebuffer"); return; }

  Serial.println();
  Serial.println("############ M3: typing-latency spike ############");
  Serial.printf("panel        : %ux%u, UC8179 (windowed refresh NOT supported)\n", kW, kH);
  Serial.printf("async refresh: %s\n",
                display.supportsAsyncRefresh() ? "YES (real overlap)" : "no (falls back to blocking)");
  Serial.println();

  // --- 1. whole-panel blocking refresh, per mode ---
  Serial.println("-- blocking whole-panel refresh --");
  Serial.printf("  FULL_REFRESH : %lu ms\n", (unsigned long)timeBlocking(EInkDisplay::FULL_REFRESH, fb, 12));
  Serial.printf("  HALF_REFRESH : %lu ms\n", (unsigned long)timeBlocking(EInkDisplay::HALF_REFRESH, fb, 13));
  Serial.printf("  FAST_REFRESH : %lu ms\n", (unsigned long)timeBlocking(EInkDisplay::FAST_REFRESH, fb, 14));
  Serial.println();

  // --- 2. async: how fast does the call hand control back? ---
  renderFakePage(fb, 15);
  uint32_t t0 = millis();
  display.displayBufferAsync(EInkDisplay::FAST_REFRESH);
  const uint32_t returnMs = millis() - t0;
  while (display.refreshBusy()) { /* spin */ }
  const uint32_t completeMs = millis() - t0;
  Serial.println("-- async FAST refresh --");
  Serial.printf("  call returns after : %lu ms   <- how long typing is blocked\n",
                (unsigned long)returnMs);
  Serial.printf("  panel settles after: %lu ms   <- when the reader sees it\n",
                (unsigned long)completeMs);
  Serial.println();

  // --- 3. the number that matters: sustained typing ---
  // Each iteration = one keystroke: mutate the buffer, fire a refresh, and
  // measure how long until we could accept the NEXT keystroke.
  Serial.println("-- simulated typing, 20 keystrokes --");
  display.waitRefreshComplete();
  renderFakePage(fb, 10);
  display.displayBuffer(EInkDisplay::FAST_REFRESH);

  Stats blocked, settle;
  for (uint16_t i = 0; i < 20; ++i) {
    typeOneChar(fb, i);
    const uint32_t k0 = millis();
    display.displayBufferAsync(EInkDisplay::FAST_REFRESH);
    blocked.add(millis() - k0);
    while (display.refreshBusy()) { /* spin */ }
    settle.add(millis() - k0);
  }
  Serial.printf("  input blocked : min %lu / avg %lu / max %lu ms\n",
                (unsigned long)blocked.lo, (unsigned long)blocked.avg(), (unsigned long)blocked.hi);
  Serial.printf("  char visible  : min %lu / avg %lu / max %lu ms\n",
                (unsigned long)settle.lo, (unsigned long)settle.avg(), (unsigned long)settle.hi);
  Serial.println();
  Serial.printf("  => sustained typing ceiling: ~%lu chars/sec\n",
                settle.avg() ? (unsigned long)(1000UL / settle.avg()) : 0UL);
  Serial.println("#################################################");
  Serial.println("Hold RIGHT 2s to boot the other slot.");
}

void bootOtherSlot() {
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!other) { Serial.println("[err] no other OTA slot"); return; }
  Serial.printf("[..] switching to %s\n", other->label);
  if (esp_ota_set_boot_partition(other) != ESP_OK) { Serial.println("[err] switch failed"); return; }
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

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  esp_ota_mark_app_valid_cancel_rollback();
  pinMode(kSwitchButton, INPUT_PULLUP);

  gPanelPromoted = freeink::applyXteinkDisplayController();
  Serial.printf("\n[boot] slot=%s controller=%s psram=%u\n",
                esp_ota_get_running_partition()->label,
                gPanelPromoted ? "UC81xx (probed)" : "SSD1677 (default)",
                (unsigned)ESP.getPsramSize());

  display.begin();
  runBenchmark();
}

void loop() {
  pollSwitchButton();
  delay(20);
}
