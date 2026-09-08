#include "slug.h"

#include <string.h>

namespace pocketx {
namespace {

// Latin-1 / Latin Extended transliterations. German umlauts expand the way
// German does it (ä -> ae), which is what a reader expects to see in a filename;
// accented vowels fold to their base letter.
const char* translit(uint32_t cp) {
  switch (cp) {
    case 0xE4: return "ae";  case 0xC4: return "ae";   // ä Ä
    case 0xF6: return "oe";  case 0xD6: return "oe";   // ö Ö
    case 0xFC: return "ue";  case 0xDC: return "ue";   // ü Ü
    case 0xDF: return "ss";                            // ß
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE5:
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC5: return "a";
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "e";
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return "i";
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF8:
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD8: return "o";
    case 0xF9: case 0xFA: case 0xFB:
    case 0xD9: case 0xDA: case 0xDB: return "u";
    case 0xE7: case 0xC7: return "c";
    case 0xF1: case 0xD1: return "n";
    case 0xFD: case 0xFF: case 0xDD: return "y";
    case 0x20AC: return "eur";                         // €
    default: return nullptr;
  }
}

uint8_t decode(const char* s, uint32_t* cp) {
  const uint8_t* p = (const uint8_t*)s;
  const uint8_t b0 = p[0];
  auto cont = [](uint8_t b) { return (b & 0xC0) == 0x80; };
  if (b0 < 0x80) { *cp = b0; return 1; }
  if ((b0 & 0xE0) == 0xC0 && cont(p[1])) { *cp = ((uint32_t)(b0 & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
  if ((b0 & 0xF0) == 0xE0 && cont(p[1]) && cont(p[2])) {
    *cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return 3;
  }
  *cp = 0xFFFD;
  return 1;
}

}  // namespace

size_t slugify(const char* title, char* out, size_t outSize, const char* fallback) {
  if (!out || outSize == 0) return 0;
  size_t n = 0;
  bool lastWasDash = true;   // leading: suppress a dash at the very start

  auto put = [&](char c) {
    if (n + 1 < outSize) out[n++] = c;
  };

  if (title) {
    for (const char* p = title; *p && n + 1 < outSize;) {
      uint32_t cp = 0;
      p += decode(p, &cp);

      if (cp >= 'A' && cp <= 'Z') cp += 32;  // lowercase

      if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
        put((char)cp);
        lastWasDash = false;
        continue;
      }
      if (const char* t = translit(cp)) {
        for (const char* q = t; *q; ++q) put(*q);
        lastWasDash = false;
        continue;
      }
      // Everything else -- spaces, punctuation, emoji, unknown scripts --
      // collapses into a single separator.
      if (!lastWasDash) {
        put('-');
        lastWasDash = true;
      }
    }
  }

  while (n > 0 && out[n - 1] == '-') --n;   // no trailing separator
  out[n] = 0;

  if (n == 0 && fallback) {
    for (const char* q = fallback; *q && n + 1 < outSize; ++q) out[n++] = *q;
    out[n] = 0;
  }
  return n;
}

}  // namespace pocketx
