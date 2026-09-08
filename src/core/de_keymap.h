#pragma once

// German (T1 / QWERTZ) keyboard layout for HID usage codes.
//
// Deliberately free of Arduino and BLE headers so it compiles for the host and
// is covered by native unit tests -- this is where correctness actually bites
// (umlauts, sharp s, AltGr, dead keys), and hardware is a terrible place to
// discover a wrong keymap.
//
// Why not the SDK's translation: freeink::hidTranslate is a US layout (shift-2
// gives '@', not '"') and it returns a single `char`, which cannot represent
// ä ö ü ß € § ° µ at all. This maps to UTF-8 byte sequences instead.

#include <stdint.h>
#include <stddef.h>

namespace pocketx {

// HID modifier bitmask, as delivered in a boot-protocol keyboard report.
enum : uint8_t {
  kModLCtrl  = 0x01,
  kModLShift = 0x02,
  kModLAlt   = 0x04,
  kModLGui   = 0x08,
  kModRCtrl  = 0x10,
  kModRShift = 0x20,
  kModRAlt   = 0x40,  // AltGr on a German keyboard
  kModRGui   = 0x80,
};

// Pending dead key. German has three: circumflex, acute and grave. They compose
// with the next keystroke rather than emitting immediately.
enum class Dead : uint8_t { None = 0, Circumflex, Acute, Grave };

// Result of translating one key press.
struct KeyText {
  char utf8[8] = {0};   // NUL-terminated; empty when the key produced no text
  uint8_t len = 0;      // bytes in utf8, excluding the terminator
  bool consumedAsDead = false;  // key armed a dead key; nothing to insert yet
};

// Translate one HID usage + modifier bitmask into text, honouring and updating
// the pending dead-key state.
//
// Returns false when the key produces no text at all (modifiers, function keys,
// navigation -- the caller handles those via freeink::SpecialKey), in which case
// `out` is left empty. A Ctrl/Gui chord also returns false: those are commands,
// not text.
bool deTranslate(uint8_t usage, uint8_t mods, Dead& dead, KeyText& out);

// True when the modifier set means "AltGr". Most keyboards send RAlt; some send
// Ctrl+Alt for the same key, so both are accepted.
bool isAltGr(uint8_t mods);

}  // namespace pocketx
