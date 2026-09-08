#include <unity.h>
#include <string.h>
#include "core/text_render.h"
#include "fonts/NotoSans261bpp.h"

using namespace pocketx;

namespace {

constexpr uint16_t kW = 200, kH = 60, kStride = kW / 8;
uint8_t g_bits[kStride * kH];
const Font& font = freeink::ui::kNotoSans261bppFont;

Canvas freshCanvas() {
  memset(g_bits, 0xFF, sizeof(g_bits));   // 0xFF = white page
  return Canvas{g_bits, kW, kH, kStride};
}

bool isBlack(const Canvas& c, int x, int y) {
  return !((c.bits[(uint32_t)y * c.stride + (x >> 3)] >> (7 - (x & 7))) & 1);
}

int countBlack(const Canvas& c) {
  int n = 0;
  for (int y = 0; y < c.height; ++y)
    for (int x = 0; x < c.width; ++x)
      if (isBlack(c, x, y)) ++n;
  return n;
}

}  // namespace

void test_utf8_decodes_ascii_and_multibyte() {
  uint32_t cp = 0;
  TEST_ASSERT_EQUAL_UINT8(1, utf8Next("A", &cp));       TEST_ASSERT_EQUAL_UINT32('A', cp);
  TEST_ASSERT_EQUAL_UINT8(2, utf8Next("\xC3\xA4", &cp)); TEST_ASSERT_EQUAL_UINT32(0xE4, cp);   // ä
  TEST_ASSERT_EQUAL_UINT8(2, utf8Next("\xC3\x9F", &cp)); TEST_ASSERT_EQUAL_UINT32(0xDF, cp);   // ß
  TEST_ASSERT_EQUAL_UINT8(3, utf8Next("\xE2\x82\xAC", &cp)); TEST_ASSERT_EQUAL_UINT32(0x20AC, cp); // €
}

// A malformed byte must consume exactly one byte, or a corrupt buffer stalls
// the render loop forever.
void test_utf8_invalid_byte_still_advances() {
  uint32_t cp = 0;
  TEST_ASSERT_EQUAL_UINT8(1, utf8Next("\xFF", &cp));
  TEST_ASSERT_EQUAL_UINT32(0xFFFD, cp);
}

void test_font_covers_german_characters() {
  // Every German character must have a non-zero advance, or it silently
  // renders as nothing -- exactly the failure the bundled ASCII font would give.
  const char* german = "\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x84\xC3\x96\xC3\x9C\xC3\x9F";  // äöüÄÖÜß
  for (const char* p = german; *p;) {
    uint32_t cp = 0;
    p += utf8Next(p, &cp);
    TEST_ASSERT_TRUE_MESSAGE(glyphAdvance(font, cp) > 0, "German glyph missing from font");
  }
}

void test_measure_is_additive() {
  const uint32_t a = measureText(font, "Hallo");
  const uint32_t b = measureText(font, " Welt");
  TEST_ASSERT_EQUAL_UINT32(a + b, measureText(font, "Hallo Welt"));
}

void test_empty_and_null_are_safe() {
  TEST_ASSERT_EQUAL_UINT32(0, measureText(font, ""));
  TEST_ASSERT_EQUAL_UINT32(0, measureText(font, nullptr));
  Canvas c = freshCanvas();
  TEST_ASSERT_TRUE(drawText(c, font, 0, 30, "").empty());
  TEST_ASSERT_EQUAL_INT(0, countBlack(c));
}

void test_draw_puts_ink_on_the_page() {
  Canvas c = freshCanvas();
  const Rect d = drawText(c, font, 4, 30, "Hallo");
  TEST_ASSERT_FALSE(d.empty());
  TEST_ASSERT_TRUE(countBlack(c) > 0);
}

void test_umlaut_draws_more_ink_than_its_base_letter() {
  // ä must differ from a: if the glyph were missing they would be identical.
  Canvas c1 = freshCanvas();
  drawText(c1, font, 4, 40, "a");
  const int plain = countBlack(c1);
  Canvas c2 = freshCanvas();
  drawText(c2, font, 4, 40, "\xC3\xA4");
  const int umlaut = countBlack(c2);
  TEST_ASSERT_TRUE_MESSAGE(umlaut > plain, "a-umlaut has no diaeresis");
}

// The damage rect drives the panel update strategy, so it must actually
// enclose every pixel that was touched.
void test_damage_rect_encloses_all_ink() {
  Canvas c = freshCanvas();
  const Rect d = drawText(c, font, 10, 40, "Gr\xC3\xB6\xC3\x9F""e");
  for (int y = 0; y < c.height; ++y)
    for (int x = 0; x < c.width; ++x)
      if (isBlack(c, x, y)) {
        TEST_ASSERT_TRUE_MESSAGE(x >= d.x && x < d.x + d.w, "ink left of damage rect");
        TEST_ASSERT_TRUE_MESSAGE(y >= d.y && y < d.y + d.h, "ink outside damage rect rows");
      }
}

void test_damage_rect_stays_inside_the_canvas() {
  Canvas c = freshCanvas();
  const Rect d = drawText(c, font, kW - 12, 40, "MMMMMMMM");  // deliberately overflows
  TEST_ASSERT_TRUE(d.x >= 0 && d.y >= 0);
  TEST_ASSERT_TRUE(d.x + d.w <= kW);
  TEST_ASSERT_TRUE(d.y + d.h <= kH);
}

// Clipping must not corrupt neighbouring memory.
void test_drawing_off_canvas_is_safe() {
  Canvas c = freshCanvas();
  drawText(c, font, -50, 40, "Hallo");
  drawText(c, font, 10, -40, "Hallo");
  drawText(c, font, 10, 500, "Hallo");
  TEST_ASSERT_TRUE(true);  // reaching here without a crash is the assertion
}

void test_fill_rect_paints_and_clears() {
  Canvas c = freshCanvas();
  fillRect(c, Rect{2, 3, 10, 5}, true);
  TEST_ASSERT_TRUE(isBlack(c, 2, 3));
  TEST_ASSERT_TRUE(isBlack(c, 11, 7));
  TEST_ASSERT_FALSE(isBlack(c, 12, 7));
  fillRect(c, Rect{2, 3, 10, 5}, false);
  TEST_ASSERT_EQUAL_INT(0, countBlack(c));
}


// --- line wrapping -----------------------------------------------------------

namespace {
// Copy a laid-out line back out so tests can compare it as a string.
const char* lineText(const char* src, const Line& l) {
  static char buf[128];
  uint32_t n = l.end - l.begin;
  if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
  memcpy(buf, src + l.begin, n);
  buf[n] = 0;
  return buf;
}
}  // namespace

void test_wrap_short_text_is_one_line() {
  Line lines[8];
  const char* s = "Hallo";
  TEST_ASSERT_EQUAL_UINT16(1, wrapText(font, s, 1000, lines, 8));
  TEST_ASSERT_EQUAL_STRING("Hallo", lineText(s, lines[0]));
}

void test_wrap_breaks_at_spaces() {
  Line lines[8];
  const char* s = "Hallo Welt";
  const uint32_t w = measureText(font, "Hallo") + 4;   // room for one word only
  const uint16_t n = wrapText(font, s, w, lines, 8);
  TEST_ASSERT_EQUAL_UINT16(2, n);
  TEST_ASSERT_EQUAL_STRING("Hallo", lineText(s, lines[0]));
  TEST_ASSERT_EQUAL_STRING("Welt", lineText(s, lines[1]));
}

void test_wrap_honours_explicit_newline() {
  Line lines[8];
  const char* s = "eins\nzwei";
  TEST_ASSERT_EQUAL_UINT16(2, wrapText(font, s, 10000, lines, 8));
  TEST_ASSERT_EQUAL_STRING("eins", lineText(s, lines[0]));
  TEST_ASSERT_EQUAL_STRING("zwei", lineText(s, lines[1]));
}

// A word wider than the line must break inside itself, not run off the page.
void test_wrap_breaks_inside_an_overlong_word() {
  Line lines[8];
  const char* s = "Donaudampfschifffahrtsgesellschaft";
  const uint16_t n = wrapText(font, s, measureText(font, "Donau"), lines, 8);
  TEST_ASSERT_TRUE(n >= 2);
  TEST_ASSERT_TRUE(lines[0].end > lines[0].begin);
}

void test_wrap_never_exceeds_the_width() {
  Line lines[16];
  const char* s = "Der Hahn kr\xC3\xA4ht fr\xC3\xBCh am Morgen und weckt das ganze Dorf";
  const uint32_t maxW = 180;
  const uint16_t n = wrapText(font, s, maxW, lines, 16);
  TEST_ASSERT_TRUE(n > 1);
  for (uint16_t i = 0; i < n; ++i) {
    char buf[128];
    uint32_t len = lines[i].end - lines[i].begin;
    memcpy(buf, s + lines[i].begin, len); buf[len] = 0;
    TEST_ASSERT_TRUE_MESSAGE(measureText(font, buf) <= maxW, "a wrapped line is wider than the limit");
  }
}

void test_wrap_handles_umlauts_in_measurement() {
  Line lines[8];
  const char* s = "Gr\xC3\xB6\xC3\x9F" "e";   // "Größe" in UTF-8
  TEST_ASSERT_EQUAL_UINT16(1, wrapText(font, s, 10000, lines, 8));
  TEST_ASSERT_EQUAL_STRING(s, lineText(s, lines[0]));
}


void test_invert_flips_pixels_and_is_its_own_inverse() {
  Canvas c = freshCanvas();
  drawText(c, font, 4, 40, "Hallo");
  const int before = countBlack(c);
  const Rect r{0, 0, 100, 50};
  invertRect(c, r);
  TEST_ASSERT_NOT_EQUAL(before, countBlack(c));
  invertRect(c, r);
  TEST_ASSERT_EQUAL_INT(before, countBlack(c));
}

void test_invert_off_canvas_is_safe() {
  Canvas c = freshCanvas();
  invertRect(c, Rect{-20, -20, 40, 40});
  invertRect(c, Rect{(int32_t)kW - 4, (int32_t)kH - 4, 40, 40});
  TEST_ASSERT_TRUE(true);
}


namespace {
uint8_t g_bits2[kStride * kH];
Canvas secondCanvas() {
  memset(g_bits2, 0xFF, sizeof(g_bits2));
  return Canvas{g_bits2, kW, kH, kStride};
}
}  // namespace

void test_diff_of_identical_canvases_is_empty() {
  Canvas a = freshCanvas();
  Canvas b = secondCanvas();
  drawText(a, font, 10, 40, "Hallo");
  memcpy(b.bits, a.bits, kStride * kH);
  TEST_ASSERT_TRUE(diffCanvas(a, b).empty());
}

// The diff must enclose every differing pixel, or the panel would be told to
// refresh too small an area and leave stale ink behind.
void test_diff_encloses_every_changed_pixel() {
  Canvas a = freshCanvas();
  Canvas b = secondCanvas();
  drawText(b, font, 10, 40, "Hallo");
  const Rect d = diffCanvas(a, b);
  TEST_ASSERT_FALSE(d.empty());
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x) {
      const bool pa = !((a.bits[(uint32_t)y * kStride + (x >> 3)] >> (7 - (x & 7))) & 1);
      const bool pb = !((b.bits[(uint32_t)y * kStride + (x >> 3)] >> (7 - (x & 7))) & 1);
      if (pa != pb) {
        TEST_ASSERT_TRUE_MESSAGE(x >= d.x && x < d.x + d.w, "changed pixel outside diff columns");
        TEST_ASSERT_TRUE_MESSAGE(y >= d.y && y < d.y + d.h, "changed pixel outside diff rows");
      }
    }
}

// The panel addresses partial-window columns in bytes, so x and w must be
// multiples of 8 or the window is rejected and the whole page repaints.
void test_diff_x_is_byte_aligned() {
  Canvas a = freshCanvas();
  Canvas b = secondCanvas();
  drawText(b, font, 13, 40, "x");
  const Rect d = diffCanvas(a, b);
  TEST_ASSERT_EQUAL_INT(0, d.x % 8);
  TEST_ASSERT_EQUAL_INT(0, d.w % 8);
}

// One typed character must not report the whole page as damaged.
void test_diff_of_a_single_line_stays_small() {
  Canvas a = freshCanvas();
  drawText(a, font, 10, 40, "Hallo");
  Canvas b = secondCanvas();
  memcpy(b.bits, a.bits, kStride * kH);
  drawText(b, font, 10, 40, "Hallo!");
  const Rect d = diffCanvas(a, b);
  TEST_ASSERT_FALSE(d.empty());
  TEST_ASSERT_TRUE_MESSAGE(d.h < 40, "one line of change reported as many rows");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_utf8_decodes_ascii_and_multibyte);
  RUN_TEST(test_utf8_invalid_byte_still_advances);
  RUN_TEST(test_font_covers_german_characters);
  RUN_TEST(test_measure_is_additive);
  RUN_TEST(test_empty_and_null_are_safe);
  RUN_TEST(test_draw_puts_ink_on_the_page);
  RUN_TEST(test_umlaut_draws_more_ink_than_its_base_letter);
  RUN_TEST(test_damage_rect_encloses_all_ink);
  RUN_TEST(test_damage_rect_stays_inside_the_canvas);
  RUN_TEST(test_drawing_off_canvas_is_safe);
  RUN_TEST(test_fill_rect_paints_and_clears);
  RUN_TEST(test_wrap_short_text_is_one_line);
  RUN_TEST(test_wrap_breaks_at_spaces);
  RUN_TEST(test_wrap_honours_explicit_newline);
  RUN_TEST(test_wrap_breaks_inside_an_overlong_word);
  RUN_TEST(test_wrap_never_exceeds_the_width);
  RUN_TEST(test_wrap_handles_umlauts_in_measurement);
  RUN_TEST(test_invert_flips_pixels_and_is_its_own_inverse);
  RUN_TEST(test_invert_off_canvas_is_safe);
  RUN_TEST(test_diff_of_identical_canvases_is_empty);
  RUN_TEST(test_diff_encloses_every_changed_pixel);
  RUN_TEST(test_diff_x_is_byte_aligned);
  RUN_TEST(test_diff_of_a_single_line_stays_small);
  return UNITY_END();
}
