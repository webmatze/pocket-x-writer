#pragma once

// Persisting the document to the SD card.
//
// Firmware-only: this is where Arduino, SdFat and the SDK live. The parts worth
// unit-testing (filename derivation) are in core/slug.*.
//
// The one rule that matters: a save must never be able to destroy the previous
// version. A writer losing a chapter to a half-written file is the worst thing
// this device could do.

#include <stdint.h>

#include "core/document.h"

namespace pocketx {

// One chapter file on the card.
struct Chapter {
  uint16_t number = 0;
  char slug[40] = {0};
  char file[64] = {0};    // basename, e.g. "03-die-groesse.md"
  uint32_t bytes = 0;
};

class Storage {
 public:
  static constexpr uint16_t kMaxChapters = 64;

  bool begin();
  bool mounted() const { return mounted_; }

  // --- book projects -------------------------------------------------------
  // Chapters live at /books/<book>/chapters/NN-slug.md, the layout the
  // book-writer tooling reads, so no conversion is needed on the Mac.
  bool openBook(const char* bookTitle);
  const char* bookSlug() const { return book_; }

  // Fill `out` with the chapters found, sorted by number. Returns how many.
  // Files that do not parse as chapters are skipped rather than guessed at.
  uint16_t listChapters(Chapter* out, uint16_t max);

  bool loadChapter(Document& doc, const Chapter& ch);
  bool saveChapter(const Document& doc, const Chapter& ch);
  // Create the next chapter after the highest existing number.
  bool createChapter(const char* title, Chapter* out);

  // Write the document to /notes/<slug>.md.
  //
  // Sequence: write a temp file, flush, read it back and compare byte for byte,
  // rotate the existing file to .bak, then rename the temp into place. A crash
  // or a pulled card at any point leaves either the previous file or its .bak
  // intact -- never a truncated current file.
  bool save(const Document& doc, const char* title);

  // Load /notes/<slug>.md into the document, replacing its contents.
  bool load(Document& doc, const char* title);

  bool exists(const char* title);
  const char* lastError() const { return err_; }

  // Path this title maps to, for logging and for the UI.
  static void pathFor(const char* title, char* out, uint32_t outSize, const char* ext = ".md");

 private:
  bool fail(const char* msg);
  void chapterPath(const Chapter& ch, char* out, uint32_t outSize, const char* ext = "") const;
  bool mounted_ = false;
  char book_[48] = {0};
  const char* err_ = "";
};

}  // namespace pocketx
