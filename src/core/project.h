#pragma once

// Book projects: chapters, word counts, and daily goals.
//
// The file naming deliberately matches what the book-writer tooling expects --
// chapters/NN-slug.md -- so a chapter written on the train can be picked up on
// the Mac without a conversion step. That compatibility is the whole point of
// choosing this layout rather than something more convenient for the firmware.
//
// Portable: no Arduino, no SD, no display. Tested on the host.

#include <stdint.h>
#include <stddef.h>

namespace pocketx {

// Words in UTF-8 text. A word is a run of characters containing at least one
// letter or digit, so Markdown scaffolding ("#", "---", "*") does not inflate a
// writer's daily count.
uint32_t countWords(const char* utf8);

// Characters, not bytes: "Größe" is 5, not 7.
uint32_t countCharacters(const char* utf8);

// Build "NN-slug.md" for a chapter, e.g. (3, "Die Größe") -> "03-die-groesse.md".
// Numbers above 99 simply widen; the zero padding keeps directory listings and
// sorting in chapter order.
size_t chapterFileName(uint16_t number, const char* title, char* out, size_t outSize);

// Parse "03-die-groesse.md" back into its number and slug. Returns false for
// anything that is not a chapter file, so stray files on the card are ignored
// rather than misread as chapters.
bool parseChapterFileName(const char* name, uint16_t* number, char* slugOut, size_t slugSize);

// Progress toward a daily goal, clamped to 0..100. A goal of 0 means "no goal"
// and always reports 0 rather than dividing by zero.
uint8_t goalPercent(uint32_t written, uint32_t target);

}  // namespace pocketx
