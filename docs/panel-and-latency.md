# This unit's panel, and what it costs to type on it

## The panel: UC8279 (X4 variant)

Determined on hardware (MAC `b8:1f:3f:d5:fa:50`). At boot,
`freeink::applyXteinkDisplayController()` probes the live display bus and
**promotes** the driver away from the profile default (SSD1677). The driver that
actually runs identifies itself in its own busy-wait tags:

```
[10783]   Wait complete:  8279x4_DRF (1 ms)
```

That is `Uc8279X4Driver` — a **UC8279**, not the SSD1677 the board profile
assumes, and not the UC8179 an earlier revision of this document claimed.

Always call `applyXteinkDisplayController()` before `display.begin()`. On this
device it is mandatory, not defensive: the profile default selects the wrong
driver.

## Measured typing latency

M3 spike, 20 simulated keystrokes, whole page of pseudo-text as the baseline
content (an all-white panel would understate the transition cost):

| | min | avg | max |
|---|---|---|---|
| **input blocked** (until the next keystroke can be accepted) | 40 ms | **75 ms** | 77 ms |
| **char visible** (until the panel settles) | 519 ms | **554 ms** | 556 ms |

The refresh loop confirms the cadence independently: one `8279x4_DRF` completion
every ~556 ms.

### Reading these numbers

They say two different things, and conflating them would lead to the wrong fix.

**Input is fine.** 75 ms blocked is a ceiling of ~13 characters/second. A fast
typist runs 80 WPM ≈ 6.7 char/s, average prose ≈ 3.3 char/s. The keyboard will
not feel like it is dropping characters.

**Visual feedback is not fine.** 554 ms from keystroke to visible character is
exactly the "the high latency is quite noticeable" complaint reported by users
of the X4 writing firmware. Half a second of nothing after every letter.

So the problem is not throughput. It is the whole-panel repaint.

## Why every keystroke repaints all 800x480

`Uc8279X4Driver` does not override `displayWindow()`. Only `Ssd1677Driver` and
`PaperMonoDriver` do. Everything else inherits `PanelDriver`'s base version:

```cpp
virtual void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool turnOff) {
  display(bus, fb, prev, RefreshMode::Fast, turnOff);   // WHOLE PANEL
}
```

Asking for a one-line window silently repaints the entire display. It compiles,
it runs, it looks correct, and it does nothing of what was intended.

## The fix: implement a real window for UC8279

The hardware supports it and the driver already speaks the commands:

```cpp
constexpr uint8_t CMD_PARTIAL_WINDOW = 0x90;  // PTL (window; stock re-issues per partial)
constexpr uint8_t CMD_PARTIAL_IN     = 0x91;  // PTIN
constexpr uint8_t CMD_PARTIAL_OUT    = 0x92;  // PTOUT
```

It already issues `PTIN` → `PTL` → refresh → `PTOUT`, just always with a
full-panel rectangle. A driver comment records that the OEM firmware does the
same dance (`PTIN -> PTL(full window, +120 gate offset)`) and warns that **stock
never issues PTIN without a PTL window**.

A real `displayWindow()` therefore means feeding that existing PTL descriptor
our x/y/w/h and slicing the framebuffer rows to match, with
`Ssd1677Driver::displayWindow()` as the reference for the byte-alignment rules
(`x % 8 == 0`, `w % 8 == 0`) and bounds checks.

### The payoff is a hypothesis, not a promise

The working assumption is that UC8279 partial refresh time scales with the
number of gate lines driven, so a 26 px line (~5.4% of 480 rows) would cost far
less than a full panel. **That is a plausible model of the hardware, not a
measured fact.** Waveform time may have a large fixed component that a small
window does not avoid.

M3b tests exactly this: drive one 26-row window at a known Y with a solid test
strip and measure. Everything else — alignment, edge cases, integration —
follows only if that number is good.

### Known risks in the implementation

- **Gate offset.** A driver comment records the OEM sequence as
  `PTIN -> PTL(full window, +120 gate offset)`. Panel gate line 0 is apparently
  not framebuffer row 0 on this glass. A window's Y must carry the same
  transform or it will refresh the wrong strip.
- **Old-plane sync.** UC8279 partial mode diffs the old plane against the new
  plane in controller RAM. This build is `EINK_DISPLAY_SINGLE_BUFFER_MODE=1`, so
  the facade passes `prev = nullptr`. If a window uploads only the new plane for
  its region, the old plane for that region must already be in sync, or the
  diff runs against stale data and the region ghosts or does not update.

If the PTL descriptor turns out to fight us, the fallback is coalescing plus a
typewriter mode at the full-panel cost — which is what the existing X4 writing
firmware ships.
