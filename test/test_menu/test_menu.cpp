#include <unity.h>
#include <string.h>

#include "core/menu.h"

using namespace pocketx;

void setUp() {}
void tearDown() {}

// --- selection and viewport -------------------------------------------------

void test_empty_list_is_inert() {
  ListState l;
  l.reset(0, 0, 5);
  TEST_ASSERT_TRUE(l.empty());
  l.next();
  l.prev();
  l.pageNext();
  TEST_ASSERT_EQUAL_UINT32(0, l.selected());
  TEST_ASSERT_EQUAL_UINT32(0, l.firstVisible());
}

void test_selection_wraps_in_both_directions() {
  ListState l;
  l.reset(3, 0, 5);
  l.prev();
  TEST_ASSERT_EQUAL_UINT32(2, l.selected());   // up from the top -> bottom
  l.next();
  TEST_ASSERT_EQUAL_UINT32(0, l.selected());   // down from the bottom -> top
}

// Wrapping is what makes one physical button enough to reach every item.
void test_one_button_can_reach_every_item() {
  ListState l;
  l.reset(4, 0, 2);
  bool seen[4] = {false, false, false, false};
  for (int i = 0; i < 4; ++i) { seen[l.selected()] = true; l.next(); }
  for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(seen[i]);
  TEST_ASSERT_EQUAL_UINT32(0, l.selected());   // back where it started
}

void test_window_follows_the_selection_downwards() {
  ListState l;
  l.reset(10, 0, 3);
  TEST_ASSERT_EQUAL_UINT32(0, l.firstVisible());
  for (int i = 0; i < 3; ++i) l.next();        // selected == 3
  TEST_ASSERT_EQUAL_UINT32(3, l.selected());
  TEST_ASSERT_EQUAL_UINT32(1, l.firstVisible());
  TEST_ASSERT_TRUE(l.selected() < l.firstVisible() + l.visibleRows());
}

void test_window_never_leaves_a_gap_at_the_bottom() {
  ListState l;
  l.reset(10, 9, 3);
  TEST_ASSERT_EQUAL_UINT32(7, l.firstVisible());   // 7,8,9 -- full window
}

void test_short_list_never_scrolls() {
  ListState l;
  l.reset(2, 0, 5);
  l.next();
  l.next();
  TEST_ASSERT_EQUAL_UINT32(0, l.firstVisible());
}

void test_paging_clamps_instead_of_wrapping() {
  ListState l;
  l.reset(10, 0, 3);
  l.pagePrev();
  TEST_ASSERT_EQUAL_UINT32(0, l.selected());   // stays put at the top
  for (int i = 0; i < 5; ++i) l.pageNext();
  TEST_ASSERT_EQUAL_UINT32(9, l.selected());   // stops at the bottom
}

void test_reset_clamps_an_out_of_range_selection() {
  ListState l;
  l.reset(3, 99, 5);
  TEST_ASSERT_EQUAL_UINT32(2, l.selected());
}

void test_opening_on_the_current_item_shows_it() {
  ListState l;
  l.reset(20, 15, 4);
  TEST_ASSERT_EQUAL_UINT32(15, l.selected());
  TEST_ASSERT_TRUE(l.firstVisible() <= 15);
  TEST_ASSERT_TRUE(15 < l.firstVisible() + l.visibleRows());
}

// --- chapter labels ---------------------------------------------------------

void test_label_is_the_first_line() {
  char b[64];
  TEST_ASSERT_TRUE(chapterLabel("Der Hahn kraehte\nund dann\n", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Der Hahn kraehte", b);
}

void test_label_strips_markdown_heading_marks() {
  char b[64];
  TEST_ASSERT_TRUE(chapterLabel("## Zweites Kapitel\n\nText", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Zweites Kapitel", b);
}

void test_label_skips_leading_blank_lines() {
  char b[64];
  TEST_ASSERT_TRUE(chapterLabel("\n\n   \n# Spaet\n", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Spaet", b);
}

void test_label_trims_trailing_space() {
  char b[64];
  TEST_ASSERT_TRUE(chapterLabel("Titel   \nmehr", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Titel", b);
}

void test_empty_chapter_has_no_label() {
  char b[64];
  TEST_ASSERT_FALSE(chapterLabel("", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
  TEST_ASSERT_FALSE(chapterLabel("\n\n  \n", b, sizeof(b)));
  TEST_ASSERT_FALSE(chapterLabel("###\n", b, sizeof(b)));
  TEST_ASSERT_FALSE(chapterLabel(nullptr, b, sizeof(b)));
}

void test_long_label_is_truncated_and_marked() {
  char b[12];
  TEST_ASSERT_TRUE(chapterLabel("Ein sehr langer Kapitelanfang", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Ein sehr..", b);
  TEST_ASSERT_TRUE(strlen(b) < sizeof(b));
}

// A cut lands wherever it lands; it should not leave a space stranded before
// the marker.
void test_truncation_does_not_leave_a_dangling_space() {
  char b[12];
  chapterLabel("Ein sehr langer Kapitelanfang", b, sizeof(b));
  const uint32_t n = (uint32_t)strlen(b);
  TEST_ASSERT_TRUE(n >= 3);
  TEST_ASSERT_NOT_EQUAL(' ', b[n - 3]);
}

// Cutting a label must not leave half a character behind: on this panel a split
// UTF-8 sequence renders as a replacement box, not as the letter.
void test_truncation_never_splits_a_utf8_character() {
  for (uint32_t size = 5; size <= 20; ++size) {
    char b[24];
    TEST_ASSERT_TRUE(chapterLabel("ÜberÄllenGipfelnÄstchen", b, size));
    // Walk it: every sequence must be complete and inside the buffer.
    uint32_t i = 0;
    const uint32_t n = (uint32_t)strlen(b);
    while (i < n) {
      const unsigned char c = (unsigned char)b[i];
      uint32_t len = 1;
      if ((c & 0xE0) == 0xC0) len = 2;
      else if ((c & 0xF0) == 0xE0) len = 3;
      else if ((c & 0xF8) == 0xF0) len = 4;
      TEST_ASSERT_TRUE_MESSAGE(i + len <= n, "UTF-8 sequence runs past the end");
      for (uint32_t k = 1; k < len; ++k)
        TEST_ASSERT_TRUE_MESSAGE(((unsigned char)b[i + k] & 0xC0) == 0x80,
                                 "continuation byte missing");
      i += len;
    }
    TEST_ASSERT_TRUE(n < size);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_list_is_inert);
  RUN_TEST(test_selection_wraps_in_both_directions);
  RUN_TEST(test_one_button_can_reach_every_item);
  RUN_TEST(test_window_follows_the_selection_downwards);
  RUN_TEST(test_window_never_leaves_a_gap_at_the_bottom);
  RUN_TEST(test_short_list_never_scrolls);
  RUN_TEST(test_paging_clamps_instead_of_wrapping);
  RUN_TEST(test_reset_clamps_an_out_of_range_selection);
  RUN_TEST(test_opening_on_the_current_item_shows_it);
  RUN_TEST(test_label_is_the_first_line);
  RUN_TEST(test_label_strips_markdown_heading_marks);
  RUN_TEST(test_label_skips_leading_blank_lines);
  RUN_TEST(test_label_trims_trailing_space);
  RUN_TEST(test_empty_chapter_has_no_label);
  RUN_TEST(test_long_label_is_truncated_and_marked);
  RUN_TEST(test_truncation_does_not_leave_a_dangling_space);
  RUN_TEST(test_truncation_never_splits_a_utf8_character);
  return UNITY_END();
}
