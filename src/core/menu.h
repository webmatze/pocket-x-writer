#pragma once

// On-device UI, the part worth testing: which item is selected and which slice
// of a list is on screen.
//
// Free of Arduino, the SDK and the display, like the rest of core/. Drawing and
// where the items come from belong to the caller.

#include <stdint.h>

namespace pocketx {

// A scrollable single-selection list.
class ListState {
 public:
  // `visible` is how many rows fit on screen; 0 is read as 1.
  void reset(uint32_t count, uint32_t selected, uint16_t visible);
  void setVisible(uint16_t rows);

  uint32_t count() const { return count_; }
  uint32_t selected() const { return sel_; }
  uint32_t firstVisible() const { return first_; }
  uint16_t visibleRows() const { return vis_; }
  bool empty() const { return count_ == 0; }

  // Movement WRAPS, deliberately. The menu has to be usable with one physical
  // button, and without wrapping there would be no way back to the top of a
  // long list once you had walked past the item you wanted.
  void next();
  void prev();

  // Paging clamps instead of wrapping: it is a keyboard gesture, where landing
  // on the far end of the list by overshooting would be a surprise.
  void pageNext();
  void pagePrev();

  void moveTo(uint32_t index);

 private:
  // Keep the selection on screen, moving the window as little as possible --
  // the same rule the editor's scroll anchor follows.
  void keepVisible();

  uint32_t count_ = 0;
  uint32_t sel_ = 0;
  uint32_t first_ = 0;
  uint16_t vis_ = 1;
};

// Derive a display label for a chapter from its own text: the first non-empty
// line, with Markdown heading marks and surrounding space stripped.
//
// This is why the chapter list needs no titles and no text entry to be useful --
// a chapter is named by what it starts with, which is what a writer recognises
// anyway. Returns false when there is nothing to show, so the caller can fall
// back to the chapter number.
//
// Truncation never splits a UTF-8 sequence, and marks itself with ".." rather
// than an ellipsis, which is outside the rasterised character range.
bool chapterLabel(const char* text, char* out, uint32_t outSize);

}  // namespace pocketx
