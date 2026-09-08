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

### Measured: the window hypothesis is WRONG

The height sweep settles it. Same content, same panel, only the PTL rectangle
changes:

| window rows | refresh | vs. full panel |
|---|---|---|
| 32 | 514 ms | **92%** |
| 64 | 517 ms | 93% |
| 128 | 523 ms | 94% |
| 240 | 533 ms | 96% |
| 480 | 556 ms | 100% |

**A 32-row window costs 92% of a full-panel repaint.** Refresh time is almost
entirely fixed. The model — "cost scales with gate lines driven" — was wrong.

The driver config says why:

```cpp
600,   // tresHeight — addressed 800x600 (480 visible)
120,   // gateOffset — visible gates start at 120 on this variant
```

The controller scans **all 600 addressed gate lines on every refresh**,
regardless of PTL. The window decides which pixels *develop*, not how long the
scan takes. So there is no windowed shortcut to lower latency on this glass.

`Uc8279X4Driver::displayWindow()` is still implemented and correct (SDK branch
`uc8279x4-display-window`), and it is kept so nobody has to rediscover this —
but the editor must not be built around it.

## What actually works: async + coalescing

A real editor never blocks on the panel. It accepts keystrokes, coalesces them
into the framebuffer, and fires a refresh only when the panel is free. Measured
at a human 5 char/s over 30 keystrokes:

| strategy | refreshes | chars/refresh | avg age | **worst age** |
|---|---|---|---|---|
| **whole-panel async + coalescing** | 10 | 3.0 | 270 ms | **477 ms** |
| windowed, per keystroke | 30 | 1.0 | 514 ms | 514 ms |

Coalescing wins outright, and it wins *because* it batches: three characters per
refresh instead of one. The windowed variant is worse because our
`displayWindow()` is synchronous — it blocks for its full 514 ms, so every
keystroke pays in full and nothing ever coalesces.

### The honest verdict

- **Input is never the bottleneck.** Async refresh returns in ~75 ms, a ~13
  char/s ceiling against a fast typist's ~6.7 char/s. The keyboard will feel
  responsive; it will not stutter or drop characters.
- **Display feedback lags: ~270 ms typical, ~477 ms worst case.** That is a real
  limitation of this hardware, not something more code removes. It is, however,
  roughly half the naive 554 ms per keystroke.
- Design the editor so the lag is tolerable rather than pretending it is absent:
  batch on word boundaries and pauses, and keep a cursor cue that does not need
  a panel refresh to feel alive.

### One untested lever

`pll = 0x0E // (0x30) — X4 Pro only` sets the panel's frame rate, and waveform
LUT phases are counted in frames. A faster PLL would shorten every refresh
proportionally. It also trades directly against transition quality and DC
balance, so it is a separate, riskier investigation — not a free win.

### Still unmeasured

FULL and HALF blocking refresh times scrolled past before the serial monitor
attached. FULL matters later for sizing the periodic ghost-clearing refresh.
