#include "project.h"

#include <stdio.h>
#include <string.h>

#include "slug.h"

namespace pocketx {
namespace {

inline bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// A byte that makes a run count as a word. Multi-byte characters always do:
// they are letters in every script we care about here.
inline bool isWordish(unsigned char c) {
  if (c >= 0x80) return true;
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

}  // namespace

uint32_t countWords(const char* utf8) {
  if (!utf8) return 0;
  uint32_t words = 0;
  const unsigned char* p = (const unsigned char*)utf8;
  while (*p) {
    while (*p && isSpace(*p)) ++p;
    if (!*p) break;
    bool hasLetter = false;
    while (*p && !isSpace(*p)) {
      if (isWordish(*p)) hasLetter = true;
      ++p;
    }
    if (hasLetter) ++words;   // "---" and "#" are scaffolding, not words
  }
  return words;
}

uint32_t countCharacters(const char* utf8) {
  if (!utf8) return 0;
  uint32_t n = 0;
  for (const unsigned char* p = (const unsigned char*)utf8; *p; ++p)
    if ((*p & 0xC0) != 0x80) ++n;   // count lead bytes only
  return n;
}

size_t chapterFileName(uint16_t number, const char* title, char* out, size_t outSize) {
  if (!out || outSize == 0) return 0;
  char slug[48];
  slugify(title, slug, sizeof(slug), "kapitel");
  const int n = snprintf(out, outSize, "%02u-%s.md", (unsigned)number, slug);
  if (n < 0) { out[0] = 0; return 0; }
  return (size_t)((size_t)n < outSize ? n : outSize - 1);
}

bool parseChapterFileName(const char* name, uint16_t* number, char* slugOut, size_t slugSize) {
  if (!name) return false;

  // Leading digits.
  size_t i = 0;
  uint32_t num = 0;
  while (name[i] >= '0' && name[i] <= '9') {
    num = num * 10 + (uint32_t)(name[i] - '0');
    ++i;
    if (num > 9999) return false;
  }
  if (i == 0 || name[i] != '-') return false;   // needs digits and a separator
  ++i;

  // Must end in .md, with at least one slug character before it.
  const size_t len = strlen(name);
  if (len < i + 4) return false;
  if (strcmp(name + len - 3, ".md") != 0) return false;

  const size_t slugLen = len - 3 - i;
  if (slugLen == 0) return false;

  if (number) *number = (uint16_t)num;
  if (slugOut && slugSize) {
    const size_t n = slugLen < slugSize - 1 ? slugLen : slugSize - 1;
    memcpy(slugOut, name + i, n);
    slugOut[n] = 0;
  }
  return true;
}

uint8_t goalPercent(uint32_t written, uint32_t target) {
  if (target == 0) return 0;
  const uint64_t pct = (uint64_t)written * 100u / target;
  return pct > 100 ? 100 : (uint8_t)pct;
}

}  // namespace pocketx
