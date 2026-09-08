#pragma once

// The document: UTF-8 text, a cursor, and undo.
//
// Free of Arduino, the SDK and the display, so it builds for the host and is
// covered by native unit tests. Everything here is about *text*; where the text
// lands on screen is the view's problem.
//
// Storage is a plain contiguous buffer, not a gap buffer. That is a deliberate
// simplification: an insert memmoves the tail, which for a book chapter of tens
// of kilobytes costs single-digit microseconds -- against a panel refresh of
// ~550 ms measured in M3. A gap buffer would add a second code path, a
// linearisation step before every render, and its own class of off-by-one bugs,
// to optimise something five orders of magnitude away from being the bottleneck.
//
// The cursor is a BYTE offset that is always on a UTF-8 character boundary.

#include <stdint.h>

namespace pocketx {

class Document {
 public:
  struct Record {
    enum Type : uint8_t { None = 0, Insert, Erase };
    Type type = None;
    uint32_t pos = 0;        // byte offset the edit applied at
    uint32_t len = 0;        // bytes inserted, or erased
    uint32_t textOff = 0;    // Erase only: offset into the arena
    bool closed = false;     // no longer accepting coalesced edits
  };

  // Undo records live in caller-provided storage so the class allocates nothing.
  struct UndoConfig {
    char* arena = nullptr;       // text of deleted runs, for re-insertion
    uint32_t arenaSize = 0;
    Record* records = nullptr;
    uint16_t maxRecords = 0;
  };

  // Two constructors rather than a defaulted UndoConfig argument: a nested
  // type's default member initializers are not usable as a default argument
  // from inside the enclosing class definition.
  Document(char* storage, uint32_t capacity);
  Document(char* storage, uint32_t capacity, const UndoConfig& undo);

  // --- content -------------------------------------------------------------
  const char* text() const { return buf_; }   // always NUL-terminated
  uint32_t size() const { return len_; }
  bool empty() const { return len_ == 0; }
  void clear();

  // --- cursor --------------------------------------------------------------
  uint32_t cursor() const { return cursor_; }
  // Snaps to the nearest character boundary at or before `byteOffset`.
  void setCursor(uint32_t byteOffset);
  void moveLeft();
  void moveRight();
  void moveToStart() { cursor_ = 0; }
  void moveToEnd() { cursor_ = len_; }
  void moveToLineStart();
  void moveToLineEnd();
  void moveWordLeft();
  void moveWordRight();

  // --- editing -------------------------------------------------------------
  // Insert UTF-8 at the cursor; the cursor ends up after the inserted text.
  bool insert(const char* utf8, uint32_t len);
  bool insert(const char* utf8);            // NUL-terminated
  bool backspace();                          // erase the character before the cursor
  bool deleteForward();                      // erase the character at the cursor

  // --- undo ----------------------------------------------------------------
  bool undo();
  bool canUndo() const { return undoCount_ > 0; }
  // Stop coalescing: the next edit starts a new undo step. Call on cursor
  // moves and when saving, so an undo does not reach across them.
  void breakUndoGroup();

  // --- housekeeping --------------------------------------------------------
  bool dirty() const { return dirty_; }
  void markClean() { dirty_ = false; }
  uint32_t capacity() const { return cap_; }

  // Byte offset of the character boundary before / after `pos`.
  uint32_t prevBoundary(uint32_t pos) const;
  uint32_t nextBoundary(uint32_t pos) const;

 private:
  bool rawInsert(uint32_t pos, const char* src, uint32_t n);
  bool rawErase(uint32_t pos, uint32_t n);
  void recordInsert(uint32_t pos, uint32_t n);
  void recordErase(uint32_t pos, const char* text, uint32_t n);
  Record* openInsertRecord();

  char* buf_;
  uint32_t cap_;
  uint32_t len_ = 0;
  uint32_t cursor_ = 0;
  bool dirty_ = false;

  UndoConfig undo_;
  uint16_t undoCount_ = 0;
  uint32_t arenaUsed_ = 0;
};

}  // namespace pocketx
