#include <unity.h>
#include <string.h>
#include "core/project.h"

using namespace pocketx;

void test_counts_plain_words() {
  TEST_ASSERT_EQUAL_UINT32(0, countWords(""));
  TEST_ASSERT_EQUAL_UINT32(0, countWords(nullptr));
  TEST_ASSERT_EQUAL_UINT32(1, countWords("Hallo"));
  TEST_ASSERT_EQUAL_UINT32(2, countWords("Hallo Welt"));
  TEST_ASSERT_EQUAL_UINT32(2, countWords("   Hallo   Welt   "));
}

void test_counts_words_across_lines() {
  TEST_ASSERT_EQUAL_UINT32(4, countWords("eins zwei\ndrei\tvier"));
}

void test_umlauts_do_not_split_words() {
  TEST_ASSERT_EQUAL_UINT32(1, countWords("Gr\xC3\xB6\xC3\x9F" "e"));
  TEST_ASSERT_EQUAL_UINT32(3, countWords("\xC3\x9C" "ber den D\xC3\xA4""chern"));
}

// A daily word goal should measure writing, not Markdown scaffolding.
void test_markdown_scaffolding_is_not_counted() {
  TEST_ASSERT_EQUAL_UINT32(2, countWords("# Kapitel Eins"));
  TEST_ASSERT_EQUAL_UINT32(0, countWords("---"));
  TEST_ASSERT_EQUAL_UINT32(3, countWords("* eins\n* zwei\n* drei"));
}

void test_counts_characters_not_bytes() {
  TEST_ASSERT_EQUAL_UINT32(5, countCharacters("Hallo"));
  TEST_ASSERT_EQUAL_UINT32(5, countCharacters("Gr\xC3\xB6\xC3\x9F" "e"));   // 7 bytes, 5 characters
  TEST_ASSERT_EQUAL_UINT32(0, countCharacters(""));
}

// The layout must match what the book-writer tooling reads.
void test_chapter_filenames_match_the_book_writer_layout() {
  char buf[64];
  chapterFileName(3, "Die Größe", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("03-die-groesse.md", buf);
  chapterFileName(12, "Der Anfang", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("12-der-anfang.md", buf);
}

void test_untitled_chapters_still_get_a_name() {
  char buf[64];
  chapterFileName(1, "", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("01-kapitel.md", buf);
}

void test_round_trips_a_chapter_filename() {
  char name[64], slug[48];
  uint16_t n = 0;
  chapterFileName(7, "Über den Dächern", name, sizeof(name));
  TEST_ASSERT_TRUE(parseChapterFileName(name, &n, slug, sizeof(slug)));
  TEST_ASSERT_EQUAL_UINT16(7, n);
  TEST_ASSERT_EQUAL_STRING("ueber-den-daechern", slug);
}

// Stray files on the card must be ignored, not misread as chapters.
void test_rejects_things_that_are_not_chapters() {
  uint16_t n = 0;
  char slug[32];
  TEST_ASSERT_FALSE(parseChapterFileName("notes.md", &n, slug, sizeof(slug)));
  TEST_ASSERT_FALSE(parseChapterFileName("03-die-groesse.txt", &n, slug, sizeof(slug)));
  TEST_ASSERT_FALSE(parseChapterFileName("03.md", &n, slug, sizeof(slug)));
  TEST_ASSERT_FALSE(parseChapterFileName("-slug.md", &n, slug, sizeof(slug)));
  TEST_ASSERT_FALSE(parseChapterFileName("03-.md", &n, slug, sizeof(slug)));
  TEST_ASSERT_FALSE(parseChapterFileName(".DS_Store", &n, slug, sizeof(slug)));
  TEST_ASSERT_FALSE(parseChapterFileName("", &n, slug, sizeof(slug)));
}

void test_parses_unpadded_and_wide_numbers() {
  uint16_t n = 0;
  char slug[32];
  TEST_ASSERT_TRUE(parseChapterFileName("7-sieben.md", &n, slug, sizeof(slug)));
  TEST_ASSERT_EQUAL_UINT16(7, n);
  TEST_ASSERT_TRUE(parseChapterFileName("104-hundertvier.md", &n, slug, sizeof(slug)));
  TEST_ASSERT_EQUAL_UINT16(104, n);
}

void test_goal_percentage() {
  TEST_ASSERT_EQUAL_UINT8(0, goalPercent(0, 1000));
  TEST_ASSERT_EQUAL_UINT8(50, goalPercent(500, 1000));
  TEST_ASSERT_EQUAL_UINT8(100, goalPercent(1000, 1000));
  TEST_ASSERT_EQUAL_UINT8(100, goalPercent(5000, 1000));   // clamped, not 500%
  TEST_ASSERT_EQUAL_UINT8(0, goalPercent(500, 0));         // no goal set: no division by zero
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_counts_plain_words);
  RUN_TEST(test_counts_words_across_lines);
  RUN_TEST(test_umlauts_do_not_split_words);
  RUN_TEST(test_markdown_scaffolding_is_not_counted);
  RUN_TEST(test_counts_characters_not_bytes);
  RUN_TEST(test_chapter_filenames_match_the_book_writer_layout);
  RUN_TEST(test_untitled_chapters_still_get_a_name);
  RUN_TEST(test_round_trips_a_chapter_filename);
  RUN_TEST(test_rejects_things_that_are_not_chapters);
  RUN_TEST(test_parses_unpadded_and_wide_numbers);
  RUN_TEST(test_goal_percentage);
  return UNITY_END();
}
