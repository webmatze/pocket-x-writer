#include "text_render.h"

namespace pocketx {
namespace {

// 4x4 ordered Bayer matrix, scaled to the 0..15 coverage range a 4bpp font
// stores. Anti-aliased edges become a stable dither pattern rather than noise,
// which matters on e-ink where a moving pattern reads as flicker.
constexpr uint8_t kBayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5},
};

inline void setPixel(const Canvas& c, int32_t x, int32_t y, bool black) {
  if (x < 0 || y < 0 || x >= c.width || y >= c.height) return;
  const uint32_t idx = (uint32_t)y * c.stride + (uint32_t)(x >> 3);
  const uint8_t bit = 0x80u >> (x & 7);
  if (black) c.bits[idx] &= (uint8_t)~bit;   // 0 = black
  else c.bits[idx] |= bit;                   // 1 = white
}

const freeink::ui::FontGlyph* findGlyph(const Font& f, uint32_t cp) {
  if (cp < f.first || cp > f.last) return nullptr;
  return &f.glyphs[cp - f.first];
}

}  // namespace

void Rect::unite(const Rect& o) {
  if (o.empty()) return;
  if (empty()) { *this = o; return; }
  const int32_t x0 = x < o.x ? x : o.x;
  const int32_t y0 = y < o.y ? y : o.y;
  const int32_t x1 = (x + w) > (o.x + o.w) ? (x + w) : (o.x + o.w);
  const int32_t y1 = (y + h) > (o.y + o.h) ? (y + h) : (o.y + o.h);
  x = x0; y = y0; w = x1 - x0; h = y1 - y0;
}

uint8_t utf8Next(const char* s, uint32_t* cp) {
  const uint8_t* p = (const uint8_t*)s;
  const uint8_t b0 = p[0];
  auto cont = [](uint8_t b) { return (b & 0xC0) == 0x80; };

  if (b0 < 0x80) { *cp = b0; return 1; }
  if ((b0 & 0xE0) == 0xC0 && cont(p[1])) {
    *cp = ((uint32_t)(b0 & 0x1F) << 6) | (p[1] & 0x3F);
    return 2;
  }
  if ((b0 & 0xF0) == 0xE0 && cont(p[1]) && cont(p[2])) {
    *cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return 3;
  }
  if ((b0 & 0xF8) == 0xF0 && cont(p[1]) && cont(p[2]) && cont(p[3])) {
    *cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
          ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return 4;
  }
  *cp = 0xFFFD;  // invalid: consume exactly one byte so callers always progress
  return 1;
}

uint16_t glyphAdvance(const Font& font, uint32_t cp) {
  const auto* g = findGlyph(font, cp);
  return g ? g->xAdvance : 0;
}

uint32_t measureText(const Font& font, const char* utf8) {
  if (!utf8) return 0;
  uint32_t total = 0;
  for (const char* p = utf8; *p;) {
    uint32_t cp = 0;
    p += utf8Next(p, &cp);
    total += glyphAdvance(font, cp);
  }
  return total;
}

void fillRect(const Canvas& c, const Rect& r, bool black) {
  for (int32_t y = r.y; y < r.y + r.h; ++y)
    for (int32_t x = r.x; x < r.x + r.w; ++x) setPixel(c, x, y, black);
}

void invertRect(const Canvas& c, const Rect& r) {
  for (int32_t y = r.y; y < r.y + r.h; ++y) {
    if (y < 0 || y >= c.height) continue;
    for (int32_t x = r.x; x < r.x + r.w; ++x) {
      if (x < 0 || x >= c.width) continue;
      c.bits[(uint32_t)y * c.stride + (uint32_t)(x >> 3)] ^= (uint8_t)(0x80u >> (x & 7));
    }
  }
}

Rect diffCanvas(const Canvas& a, const Canvas& b) {
  Rect d;
  if (!a.bits || !b.bits || a.width != b.width || a.height != b.height || a.stride != b.stride)
    return d;

  int32_t y0 = a.height, y1 = -1, x0 = a.stride, x1 = -1;
  for (uint16_t y = 0; y < a.height; ++y) {
    const uint8_t* pa = a.bits + (uint32_t)y * a.stride;
    const uint8_t* pb = b.bits + (uint32_t)y * b.stride;
    for (uint16_t xb = 0; xb < a.stride; ++xb) {
      if (pa[xb] == pb[xb]) continue;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
      if (xb < x0) x0 = xb;
      if (xb > x1) x1 = xb;
    }
  }
  if (y1 < 0) return d;   // identical

  d.x = x0 * 8;                       // byte-aligned by construction
  d.w = (x1 - x0 + 1) * 8;
  d.y = y0;
  d.h = y1 - y0 + 1;
  return d;
}

Rect drawText(const Canvas& c, const Font& font, int32_t x, int32_t baselineY, const char* utf8) {
  Rect damage;
  if (!utf8 || !c.bits) return damage;
  const bool alpha = font.bpp == 4;

  for (const char* p = utf8; *p;) {
    uint32_t cp = 0;
    p += utf8Next(p, &cp);
    const auto* g = findGlyph(font, cp);
    if (!g) continue;

    if (g->width && g->height) {
      const int32_t gx = x + g->xOffset;
      const int32_t gy = baselineY + g->yOffset;
      const uint8_t* src = font.bitmap + g->bitmapOffset;

      for (uint16_t row = 0; row < g->height; ++row) {
        for (uint16_t col = 0; col < g->width; ++col) {
          bool ink;
          if (alpha) {
            const uint32_t i = (uint32_t)row * g->width + col;
            const uint8_t byte = src[i >> 1];
            const uint8_t cov = (i & 1) ? (byte & 0x0F) : (byte >> 4);
            // Dither partial coverage; full coverage is always ink.
            ink = cov > kBayer4[(gy + row) & 3][(gx + col) & 3];
          } else {
            const uint32_t i = (uint32_t)row * g->width + col;
            ink = (src[i >> 3] >> (7 - (i & 7))) & 1;
          }
          if (ink) setPixel(c, gx + col, gy + row, true);
        }
      }
      Rect gr{gx, gy, g->width, g->height};
      damage.unite(gr);
    }
    x += g->xAdvance;
  }

  // Clip to the canvas so callers can hand the result straight to a panel.
  if (!damage.empty()) {
    const int32_t x0 = damage.x < 0 ? 0 : damage.x;
    const int32_t y0 = damage.y < 0 ? 0 : damage.y;
    int32_t x1 = damage.x + damage.w, y1 = damage.y + damage.h;
    if (x1 > c.width) x1 = c.width;
    if (y1 > c.height) y1 = c.height;
    damage = Rect{x0, y0, x1 - x0, y1 - y0};
    if (damage.w < 0) damage.w = 0;
    if (damage.h < 0) damage.h = 0;
  }
  return damage;
}

}  // namespace pocketx

namespace pocketx {

uint16_t wrapText(const Font& font, const char* utf8, uint32_t maxWidth, Line* out, uint16_t maxLines) {
  if (!utf8 || !out || !maxLines || maxWidth == 0) return 0;

  uint16_t count = 0;
  uint32_t lineStart = 0;      // byte offset where the current line begins
  uint32_t width = 0;          // pixels used so far on this line
  uint32_t lastSpace = UINT32_MAX;   // byte offset of the last space seen
  uint32_t widthAtSpace = 0;
  uint32_t i = 0;

  auto emit = [&](uint32_t end, uint32_t nextStart) {
    out[count++] = Line{lineStart, end};
    lineStart = nextStart;
    width = 0;
    lastSpace = UINT32_MAX;
  };

  while (utf8[i] && count < maxLines) {
    uint32_t cp = 0;
    const uint8_t len = utf8Next(utf8 + i, &cp);

    if (cp == '\n') {
      emit(i, i + len);
      i += len;
      continue;
    }

    const uint32_t adv = glyphAdvance(font, cp);
    if (cp == ' ') {
      lastSpace = i;
      widthAtSpace = width;
    }

    if (width + adv > maxWidth && i > lineStart) {
      if (lastSpace != UINT32_MAX) {
        // Break at the space: it belongs to neither line.
        const uint32_t breakAt = lastSpace;
        uint32_t cpAtSpace = 0;
        const uint8_t spLen = utf8Next(utf8 + breakAt, &cpAtSpace);
        (void)widthAtSpace;
        emit(breakAt, breakAt + spLen);
        i = breakAt + spLen;
      } else {
        // A single word longer than the line: break inside it rather than
        // letting it run off the page.
        emit(i, i);
      }
      continue;
    }

    width += adv;
    i += len;
  }

  if (count < maxLines && (i > lineStart || count == 0)) out[count++] = Line{lineStart, i};
  return count;
}

}  // namespace pocketx
