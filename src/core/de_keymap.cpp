#include "de_keymap.h"

namespace pocketx {
namespace {

struct Entry {
  uint8_t usage;
  const char* base;   // unshifted
  const char* shift;
  const char* altgr;  // nullptr when the key has no AltGr meaning
};

// German T1. Only keys whose meaning differs from a plain letter/digit, plus the
// letters that QWERTZ swaps. Everything else falls through to the ASCII ranges.
constexpr Entry kTable[] = {
    // QWERTZ swap: HID usage keeps its US name, the cap does not.
    {0x1C, "z", "Z", nullptr},   // US 'y'
    {0x1D, "y", "Y", nullptr},   // US 'z'

    // Digit row: German shift symbols differ from US throughout.
    {0x1E, "1", "!", nullptr},
    {0x1F, "2", "\"", "\xC2\xB2"},          // ²
    {0x20, "3", "\xC2\xA7", "\xC2\xB3"},    // § / ³
    {0x21, "4", "$", nullptr},
    {0x22, "5", "%", nullptr},
    {0x23, "6", "&", nullptr},
    {0x24, "7", "/", "{"},
    {0x25, "8", "(", "["},
    {0x26, "9", ")", "]"},
    {0x27, "0", "=", "}"},

    // Umlauts and sharp s.
    {0x2F, "\xC3\xBC", "\xC3\x9C", nullptr},  // ü Ü
    {0x33, "\xC3\xB6", "\xC3\x96", nullptr},  // ö Ö
    {0x34, "\xC3\xA4", "\xC3\x84", nullptr},  // ä Ä
    {0x2D, "\xC3\x9F", "?", "\\"},            // ß ? backslash

    {0x30, "+", "*", "~"},
    {0x31, "#", "'", nullptr},
    {0x36, ",", ";", nullptr},
    {0x37, ".", ":", nullptr},
    {0x38, "-", "_", nullptr},

    // ISO key left of Y, absent on ANSI boards.
    {0x64, "<", ">", "|"},

    // AltGr letters.
    {0x14, "q", "Q", "@"},
    {0x08, "e", "E", "\xE2\x82\xAC"},        // €
    {0x10, "m", "M", "\xC2\xB5"},            // µ
};

const Entry* find(uint8_t usage) {
  for (const auto& e : kTable)
    if (e.usage == usage) return &e;
  return nullptr;
}

void put(KeyText& out, const char* s) {
  out.len = 0;
  if (!s) { out.utf8[0] = 0; return; }
  while (s[out.len] && out.len < sizeof(out.utf8) - 1) {
    out.utf8[out.len] = s[out.len];
    ++out.len;
  }
  out.utf8[out.len] = 0;
}

// Dead-key composition. Only the combinations German keyboards actually
// produce; anything else emits the accent followed by the plain character,
// which is what a real keyboard driver does.
const char* compose(Dead d, const char* base) {
  if (!base || base[1] != 0) return nullptr;  // single ASCII letter only
  const char c = base[0];
  switch (d) {
    case Dead::Acute:
      switch (c) {
        case 'a': return "\xC3\xA1"; case 'A': return "\xC3\x81";
        case 'e': return "\xC3\xA9"; case 'E': return "\xC3\x89";
        case 'i': return "\xC3\xAD"; case 'I': return "\xC3\x8D";
        case 'o': return "\xC3\xB3"; case 'O': return "\xC3\x93";
        case 'u': return "\xC3\xBA"; case 'U': return "\xC3\x9A";
        case 'y': return "\xC3\xBD"; case 'n': return "\xC5\x84";
        default: return nullptr;
      }
    case Dead::Grave:
      switch (c) {
        case 'a': return "\xC3\xA0"; case 'A': return "\xC3\x80";
        case 'e': return "\xC3\xA8"; case 'E': return "\xC3\x88";
        case 'i': return "\xC3\xAC"; case 'I': return "\xC3\x8C";
        case 'o': return "\xC3\xB2"; case 'O': return "\xC3\x92";
        case 'u': return "\xC3\xB9"; case 'U': return "\xC3\x99";
        default: return nullptr;
      }
    case Dead::Circumflex:
      switch (c) {
        case 'a': return "\xC3\xA2"; case 'A': return "\xC3\x82";
        case 'e': return "\xC3\xAA"; case 'E': return "\xC3\x8A";
        case 'i': return "\xC3\xAE"; case 'I': return "\xC3\x8E";
        case 'o': return "\xC3\xB4"; case 'O': return "\xC3\x94";
        case 'u': return "\xC3\xBB"; case 'U': return "\xC3\x9B";
        default: return nullptr;
      }
    default: return nullptr;
  }
}

const char* deadStandalone(Dead d) {
  switch (d) {
    case Dead::Acute: return "\xC2\xB4";   // ´
    case Dead::Grave: return "`";
    case Dead::Circumflex: return "^";
    default: return "";
  }
}

}  // namespace

bool isAltGr(uint8_t mods) {
  if (mods & kModRAlt) return true;
  // Some keyboards report AltGr as Ctrl+Alt.
  const bool ctrl = mods & (kModLCtrl | kModRCtrl);
  const bool alt = mods & kModLAlt;
  return ctrl && alt;
}

bool deTranslate(uint8_t usage, uint8_t mods, Dead& dead, KeyText& out) {
  out.utf8[0] = 0;
  out.len = 0;
  out.consumedAsDead = false;

  const bool altgr = isAltGr(mods);
  // Ctrl/Gui chords are commands, not text. AltGr is exempt: on keyboards that
  // encode it as Ctrl+Alt it would otherwise swallow @ { [ ] } \ and €.
  if (!altgr && (mods & (kModLCtrl | kModRCtrl | kModLGui | kModRGui))) return false;

  const bool shift = mods & (kModLShift | kModRShift);

  // The dead keys themselves.
  if (!altgr) {
    Dead armed = Dead::None;
    if (usage == 0x35) armed = shift ? Dead::None : Dead::Circumflex;  // ^ / °
    else if (usage == 0x2E) armed = shift ? Dead::Grave : Dead::Acute; // ` / ´
    if (armed != Dead::None) {
      if (dead == armed) {           // same dead key twice -> two literal accents
        char both[8] = {0};
        const char* s = deadStandalone(armed);
        size_t n = 0;
        for (size_t i = 0; s[i] && n < sizeof(both) - 1; ++i) both[n++] = s[i];
        for (size_t i = 0; s[i] && n < sizeof(both) - 1; ++i) both[n++] = s[i];
        put(out, both);
        dead = Dead::None;
        return true;
      }
      dead = armed;
      out.consumedAsDead = true;
      return true;
    }
    if (usage == 0x35 && shift) { put(out, "\xC2\xB0"); return true; }  // °
  }

  // Resolve the key's own text.
  const char* text = nullptr;
  char ascii[2] = {0, 0};
  if (const Entry* e = find(usage)) {
    text = altgr ? e->altgr : (shift ? e->shift : e->base);
  } else if (usage >= 0x04 && usage <= 0x1D) {          // a..z
    ascii[0] = static_cast<char>((shift ? 'A' : 'a') + (usage - 0x04));
    text = ascii;
  } else if (usage == 0x2C) {
    text = " ";
  }
  if (!text) return false;

  // Apply any pending dead key.
  if (dead != Dead::None) {
    const char* composed = compose(dead, text);
    if (composed) {
      put(out, composed);
    } else if (usage == 0x2C) {
      put(out, deadStandalone(dead));   // dead key + space = the accent itself
    } else {
      char both[8] = {0};
      const char* acc = deadStandalone(dead);
      size_t n = 0;
      for (size_t i = 0; acc[i] && n < sizeof(both) - 1; ++i) both[n++] = acc[i];
      for (size_t i = 0; text[i] && n < sizeof(both) - 1; ++i) both[n++] = text[i];
      put(out, both);
    }
    dead = Dead::None;
    return true;
  }

  put(out, text);
  return out.len > 0;
}

}  // namespace pocketx
