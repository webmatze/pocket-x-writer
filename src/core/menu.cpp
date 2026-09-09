#include "core/menu.h"

namespace pocketx {

void ListState::reset(uint32_t count, uint32_t selected, uint16_t visible) {
  count_ = count;
  vis_ = visible ? visible : 1;
  sel_ = count_ ? (selected < count_ ? selected : count_ - 1) : 0;
  first_ = 0;
  keepVisible();
}

void ListState::setVisible(uint16_t rows) {
  vis_ = rows ? rows : 1;
  keepVisible();
}

void ListState::keepVisible() {
  if (!count_) { first_ = 0; return; }
  if (sel_ < first_) first_ = sel_;
  if (sel_ >= first_ + vis_) first_ = sel_ - vis_ + 1;
  // Do not leave a gap at the bottom when the list is longer than the window.
  if (count_ > vis_ && first_ > count_ - vis_) first_ = count_ - vis_;
  if (count_ <= vis_) first_ = 0;
}

void ListState::next() {
  if (!count_) return;
  sel_ = (sel_ + 1) % count_;
  keepVisible();
}

void ListState::prev() {
  if (!count_) return;
  sel_ = sel_ ? sel_ - 1 : count_ - 1;
  keepVisible();
}

void ListState::pageNext() {
  if (!count_) return;
  sel_ = sel_ + vis_ < count_ ? sel_ + vis_ : count_ - 1;
  keepVisible();
}

void ListState::pagePrev() {
  if (!count_) return;
  sel_ = sel_ > vis_ ? sel_ - vis_ : 0;
  keepVisible();
}

void ListState::moveTo(uint32_t index) {
  if (!count_) return;
  sel_ = index < count_ ? index : count_ - 1;
  keepVisible();
}

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Byte count of the UTF-8 sequence starting at `s`, so truncation can stop on a
// character boundary. Returns 1 for anything malformed, which cannot loop.
uint32_t seqLen(const char* s) {
  const unsigned char c = (unsigned char)*s;
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

}  // namespace

bool chapterLabel(const char* text, char* out, uint32_t outSize) {
  if (!out || outSize == 0) return false;
  out[0] = 0;
  if (!text) return false;

  // First line that holds something other than space and heading marks.
  const char* p = text;
  while (*p) {
    while (isSpace(*p)) ++p;
    while (*p == '#') ++p;               // "## Kapitel" -> "Kapitel"
    while (isSpace(*p)) ++p;
    if (*p && *p != '\n') break;         // found content
    if (!*p) return false;
    ++p;                                 // empty line: try the next one
  }
  if (!*p) return false;

  // The line runs to the newline, minus trailing space.
  const char* end = p;
  while (*end && *end != '\n') ++end;
  while (end > p && isSpace(end[-1])) --end;
  if (end == p) return false;

  const uint32_t len = (uint32_t)(end - p);
  if (len < outSize) {
    for (uint32_t i = 0; i < len; ++i) out[i] = p[i];
    out[len] = 0;
    return true;
  }

  // Too long: copy whole characters until ".." still fits, then mark the cut.
  const uint32_t budget = outSize - 3;   // two dots and the terminator
  uint32_t n = 0;
  while (n < len) {
    const uint32_t step = seqLen(p + n);
    if (n + step > budget) break;
    for (uint32_t i = 0; i < step; ++i) out[n + i] = p[n + i];
    n += step;
  }
  // Drop a space left dangling by the cut: "Ein sehr .." reads worse than
  // "Ein sehr..", and the space carries nothing.
  while (n > 0 && isSpace(out[n - 1])) --n;
  out[n] = '.';
  out[n + 1] = '.';
  out[n + 2] = 0;
  return n > 0;
}

}  // namespace pocketx
