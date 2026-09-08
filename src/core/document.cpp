#include "document.h"

#include <string.h>

namespace pocketx {
namespace {

inline bool isContinuation(char c) { return (c & 0xC0) == 0x80; }

// Word boundaries for word-wise cursor movement. Deliberately simple: anything
// that is not a space or an ASCII punctuation mark counts as part of a word, so
// "Größe" stays one word (its UTF-8 continuation bytes are >= 0x80 and are not
// treated as separators).
inline bool isWordByte(char c) {
  const unsigned char u = (unsigned char)c;
  if (u >= 0x80) return true;                 // any multi-byte character
  if (u == ' ' || u == '\t' || u == '\n') return false;
  return !(u >= 0x21 && u <= 0x2F) && !(u >= 0x3A && u <= 0x40) &&
         !(u >= 0x5B && u <= 0x60) && !(u >= 0x7B && u <= 0x7E);
}

}  // namespace

Document::Document(char* storage, uint32_t capacity) : buf_(storage), cap_(capacity) {
  if (buf_ && cap_) buf_[0] = 0;
}

Document::Document(char* storage, uint32_t capacity, const UndoConfig& undo)
    : buf_(storage), cap_(capacity), undo_(undo) {
  if (buf_ && cap_) buf_[0] = 0;
}

void Document::clear() {
  len_ = 0;
  cursor_ = 0;
  hasAnchor_ = false;
  undoCount_ = 0;
  arenaUsed_ = 0;
  if (buf_ && cap_) buf_[0] = 0;
  dirty_ = true;
}

uint32_t Document::prevBoundary(uint32_t pos) const {
  if (pos == 0) return 0;
  uint32_t i = pos - 1;
  while (i > 0 && isContinuation(buf_[i])) --i;
  return i;
}

uint32_t Document::nextBoundary(uint32_t pos) const {
  if (pos >= len_) return len_;
  uint32_t i = pos + 1;
  while (i < len_ && isContinuation(buf_[i])) ++i;
  return i;
}

void Document::setCursor(uint32_t byteOffset, bool extend) {
  if (byteOffset > len_) byteOffset = len_;
  // Snap back onto a character boundary so a cursor can never split an "ä".
  while (byteOffset > 0 && isContinuation(buf_[byteOffset])) --byteOffset;
  if (extend) {
    // Anchor at where the cursor was, before it moves.
    if (!hasAnchor_) { anchor_ = cursor_; hasAnchor_ = true; }
  } else {
    hasAnchor_ = false;
  }
  cursor_ = byteOffset;
  breakUndoGroup();
}

void Document::withSelection(bool extend, void (Document::*move)()) {
  if (extend) {
    if (!hasAnchor_) { anchor_ = cursor_; hasAnchor_ = true; }
  } else {
    hasAnchor_ = false;
  }
  (this->*move)();
  breakUndoGroup();
}

void Document::moveLeft(bool e)        { withSelection(e, &Document::rawMoveLeft); }
void Document::moveRight(bool e)       { withSelection(e, &Document::rawMoveRight); }
void Document::moveToStart(bool e)     { withSelection(e, &Document::rawMoveToStart); }
void Document::moveToEnd(bool e)       { withSelection(e, &Document::rawMoveToEnd); }
void Document::moveToLineStart(bool e) { withSelection(e, &Document::rawMoveToLineStart); }
void Document::moveToLineEnd(bool e)   { withSelection(e, &Document::rawMoveToLineEnd); }
void Document::moveWordLeft(bool e)    { withSelection(e, &Document::rawMoveWordLeft); }
void Document::moveWordRight(bool e)   { withSelection(e, &Document::rawMoveWordRight); }

void Document::rawMoveLeft() { cursor_ = prevBoundary(cursor_); }
void Document::rawMoveRight() { cursor_ = nextBoundary(cursor_); }
void Document::rawMoveToStart() { cursor_ = 0; }
void Document::rawMoveToEnd() { cursor_ = len_; }

void Document::rawMoveToLineStart() {
  while (cursor_ > 0 && buf_[cursor_ - 1] != '\n') --cursor_;
}

void Document::rawMoveToLineEnd() {
  while (cursor_ < len_ && buf_[cursor_] != '\n') ++cursor_;
}

void Document::rawMoveWordLeft() {
  while (cursor_ > 0 && !isWordByte(buf_[prevBoundary(cursor_)])) cursor_ = prevBoundary(cursor_);
  while (cursor_ > 0 && isWordByte(buf_[prevBoundary(cursor_)])) cursor_ = prevBoundary(cursor_);
}

void Document::rawMoveWordRight() {
  while (cursor_ < len_ && isWordByte(buf_[cursor_])) cursor_ = nextBoundary(cursor_);
  while (cursor_ < len_ && !isWordByte(buf_[cursor_])) cursor_ = nextBoundary(cursor_);
}

void Document::selectAll() {
  anchor_ = 0;
  hasAnchor_ = true;
  cursor_ = len_;
  breakUndoGroup();
}

uint32_t Document::copySelection(char* out, uint32_t outSize) const {
  if (!hasSelection() || !out || outSize == 0) return 0;
  uint32_t n = selectionLength();
  if (n > outSize - 1) n = outSize - 1;
  memcpy(out, buf_ + selectionBegin(), n);
  out[n] = 0;
  return n;
}

bool Document::deleteSelection() {
  if (!hasSelection()) return false;
  const uint32_t from = selectionBegin();
  const uint32_t n = selectionLength();
  recordErase(from, buf_ + from, n);
  if (!rawErase(from, n)) return false;
  cursor_ = from;
  hasAnchor_ = false;
  return true;
}

bool Document::rawInsert(uint32_t pos, const char* src, uint32_t n) {
  if (!buf_ || n == 0) return n == 0;
  if (len_ + n + 1 > cap_) return false;      // +1 for the NUL
  memmove(buf_ + pos + n, buf_ + pos, len_ - pos);
  memcpy(buf_ + pos, src, n);
  len_ += n;
  buf_[len_] = 0;
  dirty_ = true;
  return true;
}

bool Document::rawErase(uint32_t pos, uint32_t n) {
  if (!buf_ || n == 0 || pos + n > len_) return false;
  memmove(buf_ + pos, buf_ + pos + n, len_ - pos - n);
  len_ -= n;
  buf_[len_] = 0;
  dirty_ = true;
  return true;
}

Document::Record* Document::openInsertRecord() {
  if (!undo_.records || !undo_.maxRecords) return nullptr;
  if (undoCount_ == 0) return nullptr;
  Record& r = undo_.records[undoCount_ - 1];
  return (r.type == Record::Insert && !r.closed) ? &r : nullptr;
}

void Document::recordInsert(uint32_t pos, uint32_t n) {
  if (!undo_.records || !undo_.maxRecords) return;

  // Coalesce a run of typing into one undo step, so undo removes a word rather
  // than one letter -- what a writer expects from a typewriter-like editor.
  if (Record* open = openInsertRecord()) {
    if (open->pos + open->len == pos) {
      open->len += n;
      return;
    }
    open->closed = true;
  }

  if (undoCount_ == undo_.maxRecords) {
    // Drop the oldest record. Its arena text is not reclaimed; the arena is a
    // bounded scratch space and stale bytes are simply never referenced again.
    memmove(undo_.records, undo_.records + 1, sizeof(Record) * (undo_.maxRecords - 1));
    --undoCount_;
  }
  undo_.records[undoCount_++] = Record{Record::Insert, pos, n, 0, false};
}

void Document::recordErase(uint32_t pos, const char* text, uint32_t n) {
  if (!undo_.records || !undo_.maxRecords || !undo_.arena) return;
  if (Record* open = openInsertRecord()) open->closed = true;

  if (arenaUsed_ + n > undo_.arenaSize) {
    // Out of arena: forget history rather than record an entry we could not
    // replay. Silently keeping a wrong record would be worse than no undo.
    undoCount_ = 0;
    arenaUsed_ = 0;
    if (n > undo_.arenaSize) return;
  }
  if (undoCount_ == undo_.maxRecords) {
    memmove(undo_.records, undo_.records + 1, sizeof(Record) * (undo_.maxRecords - 1));
    --undoCount_;
  }
  memcpy(undo_.arena + arenaUsed_, text, n);
  undo_.records[undoCount_++] = Record{Record::Erase, pos, n, arenaUsed_, true};
  arenaUsed_ += n;
}

void Document::breakUndoGroup() {
  if (Record* open = openInsertRecord()) open->closed = true;
}

bool Document::insert(const char* utf8, uint32_t len) {
  if (!utf8 || len == 0) return false;
  // Typing with a selection active replaces it, as every editor does.
  if (hasSelection()) deleteSelection();
  // A run break makes undo step by word instead of swallowing a whole paragraph.
  const bool boundary = len == 1 && (utf8[0] == ' ' || utf8[0] == '\n' || utf8[0] == '\t');
  if (!rawInsert(cursor_, utf8, len)) return false;
  recordInsert(cursor_, len);
  cursor_ += len;
  if (boundary) breakUndoGroup();
  return true;
}

bool Document::insert(const char* utf8) {
  return utf8 ? insert(utf8, (uint32_t)strlen(utf8)) : false;
}

bool Document::backspace() {
  if (hasSelection()) return deleteSelection();
  if (cursor_ == 0) return false;
  const uint32_t from = prevBoundary(cursor_);
  const uint32_t n = cursor_ - from;
  recordErase(from, buf_ + from, n);
  if (!rawErase(from, n)) return false;
  cursor_ = from;
  return true;
}

bool Document::deleteWordBefore() {
  if (hasSelection()) return deleteSelection();
  if (cursor_ == 0) return false;
  uint32_t from = cursor_;
  // Trailing whitespace belongs to the deletion: pressing Ctrl+Backspace after
  // "Hallo " should remove "Hallo ", not just the space.
  while (from > 0 && !isWordByte(buf_[prevBoundary(from)])) from = prevBoundary(from);
  while (from > 0 && isWordByte(buf_[prevBoundary(from)])) from = prevBoundary(from);
  if (from == cursor_) return false;

  const uint32_t n = cursor_ - from;
  recordErase(from, buf_ + from, n);
  if (!rawErase(from, n)) return false;
  cursor_ = from;
  return true;
}

bool Document::deleteForward() {
  if (hasSelection()) return deleteSelection();
  if (cursor_ >= len_) return false;
  const uint32_t to = nextBoundary(cursor_);
  const uint32_t n = to - cursor_;
  recordErase(cursor_, buf_ + cursor_, n);
  return rawErase(cursor_, n);
}

bool Document::undo() {
  if (!undoCount_) return false;
  const Record r = undo_.records[--undoCount_];
  switch (r.type) {
    case Record::Insert:
      if (!rawErase(r.pos, r.len)) return false;
      cursor_ = r.pos;
      return true;
    case Record::Erase:
      if (!rawInsert(r.pos, undo_.arena + r.textOff, r.len)) return false;
      cursor_ = r.pos + r.len;
      arenaUsed_ = r.textOff;   // reclaim the arena tail
      return true;
    default:
      return false;
  }
}

}  // namespace pocketx
