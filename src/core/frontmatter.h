#pragma once

// Reading book metadata out of a Markdown frontmatter block.
//
//     ---
//     title: Über den Dächern
//     goal: 1000
//     ---
//
//     Free notes below, ignored by us.
//
// Flat `key: value` lines, deliberately: everything a book needs to carry —
// title, author, a word goal, a date — is flat, so nothing here has to grow into
// nesting. That keeps the parser forty lines in core/ with tests around it,
// instead of a JSON dependency or a hand-rolled JSON parser.
//
// The governing rule: **metadata must never decide whether a book can be
// opened.** A missing file, a missing block, a mangled line — each one loses a
// value and nothing else. The same instinct as the save path, where a write may
// never destroy the previous version.

#include <stdint.h>

namespace pocketx {

// Look up one key in `text`'s frontmatter block.
//
// The block is the run of lines between a leading `---` and the next `---`;
// leading blank lines before the opening fence are tolerated. Keys are matched
// case-insensitively. The value is everything after the FIRST colon, trimmed,
// with one pair of surrounding quotes removed if present.
//
// Returns false when there is no block, no such key, or an empty value. `out` is
// always NUL-terminated, and truncation never splits a UTF-8 character.
bool frontmatterValue(const char* text, const char* key, char* out, uint32_t outSize);

}  // namespace pocketx
