#include <unity.h>
#include <string.h>
#include "core/slug.h"

using namespace pocketx;

namespace {
const char* slug(const char* title) {
  static char buf[64];
  slugify(title, buf, sizeof(buf));
  return buf;
}
}  // namespace

void test_plain_title() {
  TEST_ASSERT_EQUAL_STRING("kapitel-eins", slug("Kapitel Eins"));
}

// The whole reason this exists: SdFat cannot reopen a non-ASCII long filename.
void test_umlauts_become_readable_ascii() {
  TEST_ASSERT_EQUAL_STRING("groesse", slug("Größe"));
  TEST_ASSERT_EQUAL_STRING("ueber-den-daechern", slug("Über den Dächern"));
  TEST_ASSERT_EQUAL_STRING("strasse", slug("Straße"));
}

void test_accents_fold_to_base_letters() {
  TEST_ASSERT_EQUAL_STRING("cafe", slug("Café"));
  TEST_ASSERT_EQUAL_STRING("apres-midi", slug("Après-midi"));
}

void test_punctuation_collapses_to_single_separators() {
  TEST_ASSERT_EQUAL_STRING("was-nun", slug("Was?! Nun..."));
  TEST_ASSERT_EQUAL_STRING("a-b", slug("a   ---   b"));
}

void test_no_leading_or_trailing_separator() {
  TEST_ASSERT_EQUAL_STRING("mitte", slug("  ...Mitte!!!  "));
}

void test_result_is_always_usable_as_a_filename() {
  const char* s = slug("日本語 🎉 ???");
  // Nothing left to transliterate, so the fallback must step in.
  TEST_ASSERT_EQUAL_STRING("unbenannt", s);
  for (const char* p = s; *p; ++p) {
    const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-';
    TEST_ASSERT_TRUE_MESSAGE(ok, "slug contains a character unsafe for a filename");
  }
}

void test_empty_and_null_use_the_fallback() {
  TEST_ASSERT_EQUAL_STRING("unbenannt", slug(""));
  TEST_ASSERT_EQUAL_STRING("unbenannt", slug(nullptr));
}

// Truncation must never leave a dangling separator or overflow.
void test_long_titles_are_truncated_safely() {
  char buf[12];
  const size_t n = slugify("Ein sehr sehr langer Kapiteltitel", buf, sizeof(buf));
  TEST_ASSERT_TRUE(n < sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(n, strlen(buf));
  TEST_ASSERT_TRUE(buf[n - 1] != '-');
}

void test_digits_survive() {
  TEST_ASSERT_EQUAL_STRING("kapitel-12", slug("Kapitel 12"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_plain_title);
  RUN_TEST(test_umlauts_become_readable_ascii);
  RUN_TEST(test_accents_fold_to_base_letters);
  RUN_TEST(test_punctuation_collapses_to_single_separators);
  RUN_TEST(test_no_leading_or_trailing_separator);
  RUN_TEST(test_result_is_always_usable_as_a_filename);
  RUN_TEST(test_empty_and_null_use_the_fallback);
  RUN_TEST(test_long_titles_are_truncated_safely);
  RUN_TEST(test_digits_survive);
  return UNITY_END();
}
