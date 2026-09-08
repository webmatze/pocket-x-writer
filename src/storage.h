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

class Storage {
 public:
  bool begin();
  bool mounted() const { return mounted_; }

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
  bool mounted_ = false;
  const char* err_ = "";
};

}  // namespace pocketx
