// PocketX Writer — M2 bring-up sketch.
//
// Goal: prove the toolchain, the FreeInk integration, the panel driver selection
// and the flash path in one shot — and prove that dual boot survives OUR
// firmware, not just the two images the device shipped with.
//
// Hold the RIGHT nav button for 2 s to reboot into the other OTA slot.

#include <Arduino.h>
#include <esp_ota_ops.h>

#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <XteinkDetect.h>

namespace {

// Pins come from the board profile, never hardcoded: BoardConfig::ACTIVE is
// XTEINK_X4_PRO here (the compile-time DEFAULT_DEVICE for a
// -DFREEINK_DEVICE_X4PRO=1 build). The profile carries the pinout confirmed on
// hardware by a bit-banged pin sweep: SCLK=12 MOSI=11 CS=13 DC=18 RST=14 BUSY=6.
const auto& kPins = BoardConfig::ACTIVE.display;

EInkDisplay display(kPins.sclk, kPins.mosi, kPins.cs, kPins.dc, kPins.rst,
                    kPins.busy);

// Right nav button, active-low with an internal pullup (GPIO7). GPIO0 is Left,
// but it is also the boot strap, so it is a poor choice for a hold gesture.
constexpr int kSwitchButton = 7;
constexpr uint32_t kHoldMs = 2000;

// Result of the boot-time panel probe, kept so the periodic banner can show it.
bool gPanelPromoted = false;

void reportHardware(bool promoted) {
  const auto& p = BoardConfig::ACTIVE;
  const esp_partition_t* running = esp_ota_get_running_partition();

  Serial.println();
  Serial.println("=== PocketX Writer :: bring-up ===");
  Serial.printf("Running slot : %s @ 0x%06lx\n",
                running ? running->label : "?",
                running ? (unsigned long)running->address : 0UL);
  Serial.printf("Panel        : %ux%u\n", p.displayWidth, p.displayHeight);
  Serial.printf("Controller   : %s\n",
                promoted ? "UC81xx sibling (promoted by live bus probe)"
                         : "SSD1677 (profile default, probe did not promote)");
  Serial.printf("PSRAM        : %u bytes\n", (unsigned)ESP.getPsramSize());
  Serial.printf("Flash        : %u bytes\n", (unsigned)ESP.getFlashChipSize());
  Serial.printf("Free heap    : %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println("Hold RIGHT 2s to boot the other slot.");
  Serial.println("==================================");
}

// Reboot into whichever OTA slot we are NOT running from. Asking the SDK for
// "the next update partition" avoids hardcoding app0/app1 — it is correct from
// either slot.
void bootOtherSlot() {
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!other) {
    Serial.println("[err] no other OTA slot found");
    return;
  }
  Serial.printf("[..] switching to %s @ 0x%06lx\n", other->label,
                (unsigned long)other->address);
  esp_err_t err = esp_ota_set_boot_partition(other);
  if (err != ESP_OK) {
    Serial.printf("[err] set_boot_partition failed: %s\n", esp_err_to_name(err));
    return;
  }
  Serial.println("[ok] rebooting");
  Serial.flush();
  delay(100);
  esp_restart();
}

void pollSwitchButton() {
  static uint32_t downSince = 0;
  const bool down = digitalRead(kSwitchButton) == LOW;  // active-low
  if (!down) {
    downSince = 0;
    return;
  }
  const uint32_t now = millis();
  if (downSince == 0) {
    downSince = now;
  } else if (now - downSince >= kHoldMs) {
    downSince = 0;
    bootOtherSlot();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);  // let USB CDC enumerate before the first print

  // The Arduino sdkconfig sets CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, so an
  // image the bootloader considers unproven gets rolled back after one boot
  // unless it vouches for itself. Harmless when the slot is already UNDEFINED
  // (which is what a manual otadata write leaves behind), so call it always.
  esp_ota_mark_app_valid_cancel_rollback();

  pinMode(kSwitchButton, INPUT_PULLUP);

  // Resolve which EPD silicon this production batch carries (SSD1677 vs the
  // UC8179 sibling) BEFORE begin() picks a driver. Ground truth is the live
  // display-bus probe; the OEM NVS value is diagnostics only.
  gPanelPromoted = freeink::applyXteinkDisplayController();

  reportHardware(gPanelPromoted);

  display.begin();

  // Full refresh to a clean white page: if the panel develops pixels, the
  // pinout, waveform and driver selection are all correct.
  display.clearScreen(0xFF);
  display.displayBuffer(EInkDisplay::FULL_REFRESH);

  Serial.println("[ok] display painted");
}

void loop() {
  pollSwitchButton();

  // Bring-up aid: repeat the banner so a serial monitor attached after boot
  // still sees which slot is running and which panel controller was selected.
  // The one-shot print in setup() is easy to miss -- the port disappears while
  // the device reboots and only comes back a few seconds later.
  static uint32_t lastBanner = 0;
  const uint32_t now = millis();
  if (now - lastBanner >= 15000) {
    lastBanner = now;
    reportHardware(gPanelPromoted);
  }

  delay(20);
}
