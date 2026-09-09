#include "core/frontmatter.h"

namespace pocketx {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// Start of the next line, or the terminator.
const char* nextLine(const char* p) {
  while (*p && *p != '\n') ++p;
  return *p ? p + 1 : p;
}

// True when the line at `p` is a `---` fence and nothing else.
bool isFence(const char* p) {
  while (isSpace(*p)) ++p;
  int dashes = 0;
  while (*p == '-') { ++dashes; ++p; }
  while (isSpace(*p)) ++p;
  return dashes >= 3 && (*p == '\n' || *p == 0);
}

bool isBlank(const char* p) {
  while (isSpace(*p)) ++p;
  return *p == '\n' || *p == 0;
}

uint32_t seqLen(const char* s) {
  const unsigned char c = (unsigned char)*s;
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

}  // namespace

bool frontmatterValue(const char* text, const char* key, char* out, uint32_t outSize) {
  if (!out || outSize == 0) return false;
  out[0] = 0;
  if (!text || !key || !*key) return false;

  // Find the opening fence, tolerating blank lines above it.
  const char* p = text;
  while (*p && isBlank(p) && !isFence(p)) p = nextLine(p);
  if (!isFence(p)) return false;
  p = nextLine(p);

  for (; *p && !isFence(p); p = nextLine(p)) {
    const char* line = p;
    while (isSpace(*line)) ++line;

    // Match the key against everything before the first colon.
    const char* k = key;
    const char* q = line;
    while (*k && *q && *q != ':' && lower(*q) == lower(*k)) { ++k; ++q; }
    if (*k) continue;                       // key not exhausted: no match
    while (isSpace(*q)) ++q;                // allow "title : value"
    if (*q != ':') continue;
    ++q;

    const char* v = q;
    while (isSpace(*v)) ++v;
    const char* end = v;
    while (*end && *end != '\n') ++end;
    while (end > v && isSpace(end[-1])) --end;

    // One pair of surrounding quotes, as a hand-edited file may well carry.
    if (end - v >= 2 && ((*v == '"' && end[-1] == '"') || (*v == '\'' && end[-1] == '\''))) {
      ++v;
      --end;
    }
    if (end <= v) return false;             // key present but empty

    uint32_t n = 0;
    const uint32_t len = (uint32_t)(end - v);
    while (n < len) {
      const uint32_t step = seqLen(v + n);
      if (n + step >= outSize) break;       // never split a character
      for (uint32_t i = 0; i < step; ++i) out[n + i] = v[n + i];
      n += step;
    }
    out[n] = 0;
    return n > 0;
  }
  return false;
}

}  // namespace pocketx
