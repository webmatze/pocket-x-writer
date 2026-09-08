// PocketX Writer — M2 bring-up sketch.
//
// Goal of this sketch: prove the toolchain, the FreeInk integration, the panel
// driver selection and the flash path in one shot. It paints the screen and
// reports what hardware it actually found over USB CDC serial.

#include <Arduino.h>

#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <XteinkDetect.h>

namespace {

// Pins come from the board profile, never hardcoded: BoardConfig::ACTIVE is
// XTEINK_X4_PRO here (it is the compile-time DEFAULT_DEVICE for a
// -DFREEINK_DEVICE_X4PRO=1 build). The SDK's profile carries the pinout that was
// confirmed on hardware by a bit-banged pin sweep: SCLK=12 MOSI=11 CS=13 DC=18
// RST=14 BUSY=6.
const auto& kPins = BoardConfig::ACTIVE.display;

EInkDisplay display(kPins.sclk, kPins.mosi, kPins.cs, kPins.dc, kPins.rst,
                    kPins.busy);

void reportHardware(bool promoted) {
  const auto& p = BoardConfig::ACTIVE;
  Serial.println();
  Serial.println("=== PocketX Writer :: bring-up ===");
  Serial.printf("Panel        : %ux%u\n", p.displayWidth, p.displayHeight);
  Serial.printf("Controller   : %s\n",
                promoted ? "UltraChip sibling (promoted by probe)"
                         : "profile default (SSD1677)");
  Serial.printf("PSRAM        : %u bytes\n", (unsigned)ESP.getPsramSize());
  Serial.printf("Flash        : %u bytes\n", (unsigned)ESP.getFlashChipSize());
  Serial.printf("Free heap    : %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println("==================================");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);  // let USB CDC enumerate before the first print

  // Resolve which EPD silicon this production batch carries (SSD1677 vs the
  // UC8179 sibling) BEFORE begin() picks a driver. Ground truth is the live
  // display-bus probe; the OEM NVS value is diagnostics only.
  const bool promoted = freeink::applyXteinkDisplayController();

  reportHardware(promoted);

  display.begin();

  // Full refresh to a clean white page: if the panel develops pixels, the
  // pinout, waveform and driver selection are all correct.
  display.clearScreen(0xFF);
  display.displayBuffer(EInkDisplay::FULL_REFRESH);

  Serial.println("[ok] display painted");
}

void loop() {
  delay(1000);
}
