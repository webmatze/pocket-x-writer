# This unit's panel: UC8179, not SSD1677

Determined on hardware at first boot of our own firmware (MAC `b8:1f:3f:d5:fa:50`):

```
=== PocketX Writer :: bring-up ===
Running slot : app1 @ 0x7f0000
Panel        : 800x480
Controller   : UC81xx sibling (promoted by live bus probe)
PSRAM        : 8388608 bytes
Flash        : 16777216 bytes
Free heap    : 330020 bytes
```

`freeink::applyXteinkDisplayController()` probed the live display bus and
**promoted** the driver from the profile default (SSD1677) to the UltraChip
sibling. This is a newer-batch X4 Pro carrying a **UC8179**.

Always call `applyXteinkDisplayController()` before `display.begin()`. It is not
optional on this device — the profile default would select the wrong driver.

## Consequence for typing latency (M3)

**`displayWindow()` does not do a regional refresh on this panel.**

Only `Ssd1677Driver` and `PaperMonoDriver` override `displayWindow()`. Every
other driver inherits `PanelDriver`'s base implementation:

```cpp
virtual void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool turnOff) {
  display(bus, fb, prev, RefreshMode::Fast, turnOff);   // WHOLE PANEL
}
```

So on UC8179, asking for a 1-line window silently refreshes all 800x480. It
compiles, it runs, it looks like it works — and the "only redraw the current
line" strategy the latency plan rests on quietly does nothing.

### What we do have

- **Async / deferred refresh works**: `Uc8179Driver::supportsAsyncDisplay()`
  returns `true`, so `displayBufferAsync()` fires the waveform and lets us keep
  accepting keystrokes while it runs.
- **`RefreshMode::Fast`** whole-panel partial (the driver issues PTIN/PTOUT
  around it).

### Implementing a real window for UC8179 is feasible

The driver already defines and uses the partial-window command set:

```cpp
constexpr uint8_t CMD_PARTIAL_WINDOW = 0x90;  // PTL
constexpr uint8_t CMD_PARTIAL_IN     = 0x91;  // PTIN
constexpr uint8_t CMD_PARTIAL_OUT    = 0x92;  // PTOUT
```

Today it enters partial mode across the whole panel — the code comments say so
outright: `PTIN — whole-panel partial (no 0x90 window)`. There is already a path
that writes a 9-byte window descriptor via `CMD_PARTIAL_WINDOW`, just with a
full-panel rectangle.

So a real `Uc8179Driver::displayWindow()` means feeding that descriptor our
x/y/w/h and slicing the framebuffer rows accordingly, with
`Ssd1677Driver::displayWindow()` as the reference for byte-alignment rules
(`x % 8 == 0`, `w % 8 == 0`) and bounds checks.

That is the fallback if M3 measures whole-panel Fast refresh as too slow to type
against — and it would be worth contributing back to the SDK.
