#pragma once

// Minimal UTF-8 text renderer for a 1-bit framebuffer.
//
// Free of Arduino and of FreeInkUI, so it builds for the host and is covered by
// native unit tests. The SDK's DisplayTarget can also draw text, but it pulls in
// the whole UI framework and the Arduino core; we need something the editor core
// can use in the simulator and in tests, and that reports precisely which
// rectangle changed -- the M3 measurements make damage tracking the thing the
// whole update strategy is built on.
//
// Pixel convention matches the panel framebuffer: a set bit is WHITE, a clear
// bit is BLACK, and a cleared page is 0xFF.

#include <stdint.h>

#include <FreeInkUIFont.h>  // freeink::ui::BitmapFont / FontGlyph

namespace pocketx {

struct Canvas {
  uint8_t* bits = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t stride = 0;  // bytes per row; normally width/8
};

// Inclusive-exclusive damage rectangle. Empty when w or h is 0.
struct Rect {
  int32_t x = 0, y = 0;
  int32_t w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  void unite(const Rect& o);
};

using Font = freeink::ui::BitmapFont;

// Decode one UTF-8 sequence. Returns bytes consumed (>= 1) and writes the
// codepoint; invalid bytes yield U+FFFD and consume one byte so a corrupt
// buffer can never stall the caller.
uint8_t utf8Next(const char* s, uint32_t* cp);

// Advance width of one codepoint, in pixels.
uint16_t glyphAdvance(const Font& font, uint32_t cp);

// Total advance width of a UTF-8 string.
uint32_t measureText(const Font& font, const char* utf8);

// Draw UTF-8 text with its baseline at `baselineY`, pen starting at `x`.
// Returns the rectangle actually touched, clipped to the canvas.
Rect drawText(const Canvas& canvas, const Font& font, int32_t x, int32_t baselineY, const char* utf8);

// Fill a rectangle. `black` false paints white (the page colour).
void fillRect(const Canvas& canvas, const Rect& r, bool black);

}  // namespace pocketx

namespace pocketx {

// One laid-out line: byte offsets into the source buffer, [begin, end).
struct Line {
  uint32_t begin = 0;
  uint32_t end = 0;   // excludes the trailing space or newline
};

// Break UTF-8 text into lines that fit `maxWidth` pixels.
//
// Breaks at spaces where it can and mid-word only when a single word is wider
// than the line, so a long URL cannot push text off the page. An explicit '\n'
// always starts a new line. Writes at most `maxLines` and returns how many were
// produced.
uint16_t wrapText(const Font& font, const char* utf8, uint32_t maxWidth, Line* out, uint16_t maxLines);

}  // namespace pocketx
