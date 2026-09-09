#include <unity.h>
#include <string.h>

#include "core/frontmatter.h"

using namespace pocketx;

void setUp() {}
void tearDown() {}

static const char* kBook =
    "---\n"
    "title: Über den Dächern\n"
    "goal: 1000\n"
    "author: Mathias\n"
    "---\n"
    "\n"
    "Freie Notizen, die uns nichts angehen.\n";

void test_reads_a_value() {
  char b[64];
  TEST_ASSERT_TRUE(frontmatterValue(kBook, "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Über den Dächern", b);
  TEST_ASSERT_TRUE(frontmatterValue(kBook, "goal", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("1000", b);
}

void test_key_matching_ignores_case() {
  char b[64];
  TEST_ASSERT_TRUE(frontmatterValue(kBook, "TITLE", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Über den Dächern", b);
}

void test_absent_key_is_not_an_error() {
  char b[64];
  TEST_ASSERT_FALSE(frontmatterValue(kBook, "genre", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
}

// A key must match in full: "tit" is not "title", and "titlepage" is not either.
void test_key_must_match_completely() {
  char b[64];
  TEST_ASSERT_FALSE(frontmatterValue(kBook, "tit", b, sizeof(b)));
  TEST_ASSERT_FALSE(frontmatterValue(kBook, "titlepage", b, sizeof(b)));
}

void test_body_below_the_block_is_ignored() {
  char b[64];
  const char* t = "---\ntitle: Echt\n---\ntitle: Falsch\n";
  TEST_ASSERT_TRUE(frontmatterValue(t, "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Echt", b);
}

// Everything after the FIRST colon belongs to the value -- book titles contain
// colons more often than not.
void test_value_may_contain_a_colon() {
  char b[64];
  const char* t = "---\ntitle: Der Fall: Ein Roman\n---\n";
  TEST_ASSERT_TRUE(frontmatterValue(t, "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Der Fall: Ein Roman", b);
}

void test_surrounding_quotes_are_dropped() {
  char b[64];
  TEST_ASSERT_TRUE(frontmatterValue("---\ntitle: \"Zitiert\"\n---\n", "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Zitiert", b);
  TEST_ASSERT_TRUE(frontmatterValue("---\ntitle: 'Einfach'\n---\n", "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Einfach", b);
}

void test_whitespace_around_key_and_value_is_trimmed() {
  char b[64];
  TEST_ASSERT_TRUE(frontmatterValue("---\n   title  :   Luftig   \n---\n", "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Luftig", b);
}

void test_blank_lines_above_the_fence_are_tolerated() {
  char b[64];
  TEST_ASSERT_TRUE(frontmatterValue("\n\n---\ntitle: Spaet\n---\n", "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Spaet", b);
}

// The governing rule: a damaged file loses a value, never a book.
void test_damage_costs_a_value_not_the_book() {
  char b[64];
  TEST_ASSERT_FALSE(frontmatterValue("", "title", b, sizeof(b)));
  TEST_ASSERT_FALSE(frontmatterValue(nullptr, "title", b, sizeof(b)));
  TEST_ASSERT_FALSE(frontmatterValue("kein Frontmatter hier\n", "title", b, sizeof(b)));
  TEST_ASSERT_FALSE(frontmatterValue("---\ntitle:\n---\n", "title", b, sizeof(b)));
  TEST_ASSERT_FALSE(frontmatterValue("---\nMuell ohne Doppelpunkt\n---\n", "title", b, sizeof(b)));
  // An unterminated block still yields what it holds.
  TEST_ASSERT_TRUE(frontmatterValue("---\ntitle: Offen\n", "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Offen", b);
}

void test_a_broken_line_does_not_hide_the_next_one() {
  char b[64];
  const char* t = "---\nkaputt\ngoal: 500\ntitle: Da\n---\n";
  TEST_ASSERT_TRUE(frontmatterValue(t, "title", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("Da", b);
  TEST_ASSERT_TRUE(frontmatterValue(t, "goal", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("500", b);
}

void test_truncation_never_splits_a_utf8_character() {
  for (uint32_t size = 4; size <= 24; ++size) {
    char b[32];
    frontmatterValue(kBook, "title", b, size);
    uint32_t i = 0;
    const uint32_t n = (uint32_t)strlen(b);
    TEST_ASSERT_TRUE(n < size);
    while (i < n) {
      const unsigned char c = (unsigned char)b[i];
      uint32_t len = 1;
      if ((c & 0xE0) == 0xC0) len = 2;
      else if ((c & 0xF0) == 0xE0) len = 3;
      else if ((c & 0xF8) == 0xF0) len = 4;
      TEST_ASSERT_TRUE_MESSAGE(i + len <= n, "UTF-8 sequence runs past the end");
      i += len;
    }
  }
}


// --- writing ----------------------------------------------------------------
//
// The promise the file makes is that the notes below the block belong to the
// writer. Every one of these is really about that.

void test_set_replaces_an_existing_key() {
  char b[512];
  TEST_ASSERT_TRUE(frontmatterSet(kBook, "title", "Neuer Titel", b, sizeof(b)));
  char v[64];
  TEST_ASSERT_TRUE(frontmatterValue(b, "title", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("Neuer Titel", v);
  // The other keys are untouched.
  TEST_ASSERT_TRUE(frontmatterValue(b, "goal", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("1000", v);
  TEST_ASSERT_TRUE(frontmatterValue(b, "author", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("Mathias", v);
}

void test_set_keeps_the_notes_below_the_block() {
  char b[512];
  TEST_ASSERT_TRUE(frontmatterSet(kBook, "open", "3", b, sizeof(b)));
  TEST_ASSERT_NOT_NULL(strstr(b, "Freie Notizen, die uns nichts angehen."));
}

void test_set_adds_a_key_that_was_not_there() {
  char b[512];
  TEST_ASSERT_TRUE(frontmatterSet(kBook, "open", "7", b, sizeof(b)));
  char v[64];
  TEST_ASSERT_TRUE(frontmatterValue(b, "open", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("7", v);
  TEST_ASSERT_TRUE(frontmatterValue(b, "title", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("Über den Dächern", v);
}

void test_set_creates_a_block_when_there_is_none() {
  char b[512];
  TEST_ASSERT_TRUE(frontmatterSet("Nur Notizen, kein Block.\n", "title", "Gefunden", b, sizeof(b)));
  char v[64];
  TEST_ASSERT_TRUE(frontmatterValue(b, "title", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("Gefunden", v);
  TEST_ASSERT_NOT_NULL(strstr(b, "Nur Notizen, kein Block."));
}

void test_set_on_empty_input_yields_a_readable_block() {
  char b[256];
  TEST_ASSERT_TRUE(frontmatterSet("", "title", "Leer gestartet", b, sizeof(b)));
  char v[64];
  TEST_ASSERT_TRUE(frontmatterValue(b, "title", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("Leer gestartet", v);
}

void test_set_closes_an_unterminated_block() {
  char b[256];
  TEST_ASSERT_TRUE(frontmatterSet("---\ntitle: Offen\n", "open", "2", b, sizeof(b)));
  char v[64];
  TEST_ASSERT_TRUE(frontmatterValue(b, "open", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("2", v);
  TEST_ASSERT_TRUE(frontmatterValue(b, "title", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("Offen", v);
}

// Round-tripping must be stable: writing the same value twice cannot keep
// growing the file with duplicate lines.
void test_setting_twice_does_not_duplicate_the_key() {
  char a[512], b[512];
  TEST_ASSERT_TRUE(frontmatterSet(kBook, "open", "3", a, sizeof(a)));
  TEST_ASSERT_TRUE(frontmatterSet(a, "open", "3", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING(a, b);
}

// Rather no change at all than a half-written metadata file.
void test_too_small_a_buffer_yields_nothing_not_a_fragment() {
  char b[16];
  TEST_ASSERT_FALSE(frontmatterSet(kBook, "title", "Viel zu lang für den Puffer", b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("", b);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reads_a_value);
  RUN_TEST(test_key_matching_ignores_case);
  RUN_TEST(test_absent_key_is_not_an_error);
  RUN_TEST(test_key_must_match_completely);
  RUN_TEST(test_body_below_the_block_is_ignored);
  RUN_TEST(test_value_may_contain_a_colon);
  RUN_TEST(test_surrounding_quotes_are_dropped);
  RUN_TEST(test_whitespace_around_key_and_value_is_trimmed);
  RUN_TEST(test_blank_lines_above_the_fence_are_tolerated);
  RUN_TEST(test_damage_costs_a_value_not_the_book);
  RUN_TEST(test_a_broken_line_does_not_hide_the_next_one);
  RUN_TEST(test_truncation_never_splits_a_utf8_character);
  RUN_TEST(test_set_replaces_an_existing_key);
  RUN_TEST(test_set_keeps_the_notes_below_the_block);
  RUN_TEST(test_set_adds_a_key_that_was_not_there);
  RUN_TEST(test_set_creates_a_block_when_there_is_none);
  RUN_TEST(test_set_on_empty_input_yields_a_readable_block);
  RUN_TEST(test_set_closes_an_unterminated_block);
  RUN_TEST(test_setting_twice_does_not_duplicate_the_key);
  RUN_TEST(test_too_small_a_buffer_yields_nothing_not_a_fragment);
  return UNITY_END();
}
