#include <unity.h>
#include <string.h>
#include "core/de_keymap.h"

using namespace pocketx;

namespace {

// Translate one key and return the produced UTF-8 as a comparable string.
const char* type(uint8_t usage, uint8_t mods, Dead& dead) {
  static KeyText out;
  deTranslate(usage, mods, dead, out);
  return out.utf8;
}

const char* typeFresh(uint8_t usage, uint8_t mods = 0) {
  static Dead dead = Dead::None;
  dead = Dead::None;
  return type(usage, mods, dead);
}

void assertKey(const char* expected, uint8_t usage, uint8_t mods = 0) {
  TEST_ASSERT_EQUAL_STRING(expected, typeFresh(usage, mods));
}

}  // namespace

void test_letters_are_plain_ascii() {
  assertKey("a", 0x04);
  assertKey("A", 0x04, kModLShift);
  assertKey(" ", 0x2C);
}

// The defining feature of QWERTZ: the two keys trade places.
void test_qwertz_swaps_y_and_z() {
  assertKey("z", 0x1C);   // US 'y' position
  assertKey("y", 0x1D);   // US 'z' position
  assertKey("Z", 0x1C, kModLShift);
  assertKey("Y", 0x1D, kModLShift);
}

void test_umlauts_and_sharp_s() {
  assertKey("\xC3\xA4", 0x34);              // ä
  assertKey("\xC3\x84", 0x34, kModLShift);  // Ä
  assertKey("\xC3\xB6", 0x33);              // ö
  assertKey("\xC3\xBC", 0x2F);              // ü
  assertKey("\xC3\x9F", 0x2D);              // ß
}

// The US table would give '@' here. This is the regression that matters.
void test_digit_row_uses_german_shift_symbols() {
  assertKey("\"", 0x1F, kModLShift);            // shift-2 is a quote, not @
  assertKey("\xC2\xA7", 0x20, kModLShift);      // §
  assertKey("/", 0x24, kModLShift);
  assertKey("=", 0x27, kModLShift);
}

void test_altgr_symbols() {
  assertKey("@", 0x14, kModRAlt);                  // AltGr+Q
  assertKey("\xE2\x82\xAC", 0x08, kModRAlt);       // AltGr+E = €
  assertKey("{", 0x24, kModRAlt);
  assertKey("[", 0x25, kModRAlt);
  assertKey("\\", 0x2D, kModRAlt);
}

// Keyboards that encode AltGr as Ctrl+Alt must still produce symbols, not
// swallow them as a command chord.
void test_altgr_as_ctrl_alt_is_accepted() {
  assertKey("@", 0x14, kModLCtrl | kModLAlt);
}

void test_ctrl_chords_produce_no_text() {
  KeyText out;
  Dead dead = Dead::None;
  TEST_ASSERT_FALSE(deTranslate(0x04, kModLCtrl, dead, out));  // Ctrl+A
  TEST_ASSERT_EQUAL_UINT8(0, out.len);
}

void test_dead_acute_composes() {
  Dead dead = Dead::None;
  KeyText out;
  TEST_ASSERT_TRUE(deTranslate(0x2E, 0, dead, out));   // ´
  TEST_ASSERT_TRUE(out.consumedAsDead);
  TEST_ASSERT_EQUAL_UINT8(0, out.len);                 // nothing inserted yet
  TEST_ASSERT_EQUAL_STRING("\xC3\xA9", type(0x08, 0, dead));  // é
  TEST_ASSERT_EQUAL(Dead::None, dead);
}

void test_dead_key_then_space_yields_the_accent() {
  Dead dead = Dead::None;
  KeyText out;
  deTranslate(0x2E, 0, dead, out);
  TEST_ASSERT_EQUAL_STRING("\xC2\xB4", type(0x2C, 0, dead));  // ´
}

// Accent + a letter it cannot combine with emits both, like a real driver.
void test_dead_key_with_noncomposable_emits_both() {
  Dead dead = Dead::None;
  KeyText out;
  deTranslate(0x35, 0, dead, out);                      // ^
  TEST_ASSERT_EQUAL_STRING("^k", type(0x0E, 0, dead));
}

void test_circumflex_composes_and_shift_gives_degree() {
  Dead dead = Dead::None;
  KeyText out;
  deTranslate(0x35, 0, dead, out);
  TEST_ASSERT_EQUAL_STRING("\xC3\xAA", type(0x08, 0, dead));  // ê
  assertKey("\xC2\xB0", 0x35, kModLShift);                    // °
}

void test_iso_key_left_of_z() {
  assertKey("<", 0x64);
  assertKey(">", 0x64, kModLShift);
  assertKey("|", 0x64, kModRAlt);
}

// HID usages for a..z, so the sentence tests read as text rather than hex.
constexpr uint8_t L(char c) { return 0x04 + (c - 'a'); }

// Everything a German writer needs, spelled out end to end: capitals, an
// umlaut, a sharp s, and the shift-2 quote the US table would turn into '@'.
void test_types_a_german_sentence() {
  struct { uint8_t usage; uint8_t mods; } keys[] = {
      {L('h'), kModLShift}, {L('a'), 0}, {L('h'), 0}, {L('n'), 0},   // Hahn
      {0x2C, 0},
      {L('f'), 0}, {0x2F, 0}, {L('h'), 0}, {L('r'), 0}, {L('t'), 0}, // führt
      {0x2C, 0},
      {0x1F, kModLShift},                                            // "
      {L('g'), kModLShift}, {L('r'), 0}, {0x33, 0}, {0x2D, 0}, {L('e'), 0},  // Größe
      {0x1F, kModLShift},                                            // "
  };
  char buf[64] = {0};
  size_t n = 0;
  Dead dead = Dead::None;
  KeyText out;
  for (auto& k : keys) {
    if (deTranslate(k.usage, k.mods, dead, out))
      for (uint8_t i = 0; i < out.len; ++i) buf[n++] = out.utf8[i];
  }
  TEST_ASSERT_EQUAL_STRING("Hahn f\xC3\xBChrt \"Gr\xC3\xB6\xC3\x9F""e\"", buf);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_letters_are_plain_ascii);
  RUN_TEST(test_qwertz_swaps_y_and_z);
  RUN_TEST(test_umlauts_and_sharp_s);
  RUN_TEST(test_digit_row_uses_german_shift_symbols);
  RUN_TEST(test_altgr_symbols);
  RUN_TEST(test_altgr_as_ctrl_alt_is_accepted);
  RUN_TEST(test_ctrl_chords_produce_no_text);
  RUN_TEST(test_dead_acute_composes);
  RUN_TEST(test_dead_key_then_space_yields_the_accent);
  RUN_TEST(test_dead_key_with_noncomposable_emits_both);
  RUN_TEST(test_circumflex_composes_and_shift_gives_degree);
  RUN_TEST(test_iso_key_left_of_z);
  RUN_TEST(test_types_a_german_sentence);
  return UNITY_END();
}
