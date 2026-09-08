#include <unity.h>
#include <string.h>
#include "core/document.h"

using namespace pocketx;

namespace {

constexpr uint32_t kCap = 1024;
constexpr uint32_t kArena = 256;
constexpr uint16_t kRecords = 32;

char g_buf[kCap];
char g_arena[kArena];
Document::Record g_records[kRecords];

Document makeDoc() {
  Document::UndoConfig u;
  u.arena = g_arena;
  u.arenaSize = kArena;
  u.records = g_records;
  u.maxRecords = kRecords;
  memset(g_buf, 0, sizeof(g_buf));
  memset(g_records, 0, sizeof(g_records));
  return Document(g_buf, kCap, u);
}

void type(Document& d, const char* s) {
  for (const char* p = s; *p;) {
    // Insert one whole UTF-8 character at a time, the way key events arrive.
    uint32_t n = 1;
    while ((p[n] & 0xC0) == 0x80) ++n;
    d.insert(p, n);
    p += n;
  }
}

}  // namespace

void test_starts_empty() {
  Document d = makeDoc();
  TEST_ASSERT_TRUE(d.empty());
  TEST_ASSERT_EQUAL_UINT32(0, d.cursor());
  TEST_ASSERT_EQUAL_STRING("", d.text());
  TEST_ASSERT_FALSE(d.dirty());
}

void test_insert_advances_the_cursor() {
  Document d = makeDoc();
  d.insert("Hallo");
  TEST_ASSERT_EQUAL_STRING("Hallo", d.text());
  TEST_ASSERT_EQUAL_UINT32(5, d.cursor());
  TEST_ASSERT_TRUE(d.dirty());
}

void test_insert_happens_at_the_cursor_not_the_end() {
  Document d = makeDoc();
  d.insert("Halo");
  d.setCursor(3);
  d.insert("l");
  TEST_ASSERT_EQUAL_STRING("Hallo", d.text());
  TEST_ASSERT_EQUAL_UINT32(4, d.cursor());
}

// A cursor must never land inside a multi-byte character.
void test_cursor_snaps_to_character_boundaries() {
  Document d = makeDoc();
  type(d, "\xC3\xA4\xC3\xB6");         // äö, four bytes, two characters
  d.setCursor(1);                       // inside the first 'ä'
  TEST_ASSERT_EQUAL_UINT32(0, d.cursor());
  d.setCursor(3);                       // inside the 'ö'
  TEST_ASSERT_EQUAL_UINT32(2, d.cursor());
}

void test_arrow_keys_step_by_character_not_byte() {
  Document d = makeDoc();
  type(d, "a\xC3\xA4""b");             // a ä b -> 4 bytes, 3 characters
  d.moveToStart();
  d.moveRight();  TEST_ASSERT_EQUAL_UINT32(1, d.cursor());
  d.moveRight();  TEST_ASSERT_EQUAL_UINT32(3, d.cursor());   // skipped both ä bytes
  d.moveRight();  TEST_ASSERT_EQUAL_UINT32(4, d.cursor());
  d.moveRight();  TEST_ASSERT_EQUAL_UINT32(4, d.cursor());   // clamps at the end
  d.moveLeft();   TEST_ASSERT_EQUAL_UINT32(3, d.cursor());
  d.moveLeft();   TEST_ASSERT_EQUAL_UINT32(1, d.cursor());
  d.moveLeft();   TEST_ASSERT_EQUAL_UINT32(0, d.cursor());
  d.moveLeft();   TEST_ASSERT_EQUAL_UINT32(0, d.cursor());   // clamps at the start
}

void test_backspace_removes_a_whole_character() {
  Document d = makeDoc();
  type(d, "Gr\xC3\xB6\xC3\x9F""e");    // Größe
  d.backspace();                        // 'e'
  d.backspace();                        // 'ß' - both bytes
  TEST_ASSERT_EQUAL_STRING("Gr\xC3\xB6", d.text());
}

void test_backspace_at_the_start_is_a_no_op() {
  Document d = makeDoc();
  d.insert("x");
  d.moveToStart();
  TEST_ASSERT_FALSE(d.backspace());
  TEST_ASSERT_EQUAL_STRING("x", d.text());
}

void test_delete_forward_removes_a_whole_character() {
  Document d = makeDoc();
  type(d, "\xC3\xA4""b");
  d.moveToStart();
  TEST_ASSERT_TRUE(d.deleteForward());
  TEST_ASSERT_EQUAL_STRING("b", d.text());
}

void test_line_start_and_end() {
  Document d = makeDoc();
  d.insert("eins\nzwei\ndrei");
  d.setCursor(7);                       // inside "zwei"
  d.moveToLineStart(); TEST_ASSERT_EQUAL_UINT32(5, d.cursor());
  d.moveToLineEnd();   TEST_ASSERT_EQUAL_UINT32(9, d.cursor());
}

void test_word_movement_treats_umlauts_as_word_characters() {
  Document d = makeDoc();
  type(d, "Gr\xC3\xB6\xC3\x9F""e ist gut");
  d.moveToStart();
  d.moveWordRight();
  // Must land after "Größe" plus its space, not inside the multi-byte run.
  TEST_ASSERT_EQUAL_UINT32(8, d.cursor());
  d.moveWordLeft();
  TEST_ASSERT_EQUAL_UINT32(0, d.cursor());
}

void test_capacity_is_respected() {
  char small[8];
  Document d(small, sizeof(small));
  TEST_ASSERT_TRUE(d.insert("abcdefg"));   // 7 bytes + NUL fills it exactly
  TEST_ASSERT_FALSE(d.insert("h"));
  TEST_ASSERT_EQUAL_STRING("abcdefg", d.text());
}

// --- undo --------------------------------------------------------------------

void test_undo_of_a_typing_run_removes_the_whole_word() {
  Document d = makeDoc();
  type(d, "Hallo");
  TEST_ASSERT_TRUE(d.canUndo());
  TEST_ASSERT_TRUE(d.undo());
  TEST_ASSERT_EQUAL_STRING("", d.text());
  TEST_ASSERT_EQUAL_UINT32(0, d.cursor());
}

// A space ends the run, so undo steps word by word rather than swallowing
// everything typed since the document was opened.
void test_undo_steps_word_by_word() {
  Document d = makeDoc();
  type(d, "Hallo Welt");
  d.undo();
  TEST_ASSERT_EQUAL_STRING("Hallo ", d.text());
  d.undo();
  TEST_ASSERT_EQUAL_STRING("", d.text());
}

void test_undo_restores_deleted_text() {
  Document d = makeDoc();
  type(d, "Hallo");
  d.breakUndoGroup();
  d.backspace();
  TEST_ASSERT_EQUAL_STRING("Hall", d.text());
  TEST_ASSERT_TRUE(d.undo());
  TEST_ASSERT_EQUAL_STRING("Hallo", d.text());
}

void test_undo_restores_a_deleted_umlaut_intact() {
  Document d = makeDoc();
  type(d, "Gr\xC3\xB6\xC3\x9F""e");
  d.breakUndoGroup();
  d.backspace();       // 'e'
  d.backspace();       // 'ß'
  d.undo();
  d.undo();
  TEST_ASSERT_EQUAL_STRING("Gr\xC3\xB6\xC3\x9F" "e", d.text());
}

void test_cursor_movement_breaks_the_undo_group() {
  Document d = makeDoc();
  type(d, "ab");
  d.moveLeft();        // ends the run
  d.moveRight();
  type(d, "cd");
  d.undo();
  TEST_ASSERT_EQUAL_STRING("ab", d.text());
}

void test_undo_on_an_empty_history_is_a_no_op() {
  Document d = makeDoc();
  TEST_ASSERT_FALSE(d.canUndo());
  TEST_ASSERT_FALSE(d.undo());
}

void test_document_without_undo_storage_still_edits() {
  char buf[64];
  Document d(buf, sizeof(buf));       // no UndoConfig at all
  d.insert("Hallo");
  TEST_ASSERT_EQUAL_STRING("Hallo", d.text());
  TEST_ASSERT_FALSE(d.canUndo());
  TEST_ASSERT_FALSE(d.undo());
  TEST_ASSERT_TRUE(d.backspace());
  TEST_ASSERT_EQUAL_STRING("Hall", d.text());
}

void test_dirty_flag_tracks_edits() {
  Document d = makeDoc();
  TEST_ASSERT_FALSE(d.dirty());
  d.insert("x");
  TEST_ASSERT_TRUE(d.dirty());
  d.markClean();
  TEST_ASSERT_FALSE(d.dirty());
  d.backspace();
  TEST_ASSERT_TRUE(d.dirty());
}


void test_ctrl_backspace_deletes_a_whole_word() {
  Document d = makeDoc();
  type(d, "Hallo Welt");
  TEST_ASSERT_TRUE(d.deleteWordBefore());
  TEST_ASSERT_EQUAL_STRING("Hallo ", d.text());
  TEST_ASSERT_TRUE(d.deleteWordBefore());
  TEST_ASSERT_EQUAL_STRING("", d.text());
}

// One press should remove one word, not just the space that follows it.
void test_ctrl_backspace_eats_trailing_space_with_the_word() {
  Document d = makeDoc();
  type(d, "eins zwei   ");
  d.deleteWordBefore();
  TEST_ASSERT_EQUAL_STRING("eins ", d.text());
}

void test_ctrl_backspace_keeps_umlauts_whole() {
  Document d = makeDoc();
  type(d, "Der Gr\xC3\xB6\xC3\x9F" "e");
  d.deleteWordBefore();
  TEST_ASSERT_EQUAL_STRING("Der ", d.text());
}

void test_ctrl_backspace_at_the_start_is_a_no_op() {
  Document d = makeDoc();
  type(d, "x");
  d.moveToStart();
  TEST_ASSERT_FALSE(d.deleteWordBefore());
  TEST_ASSERT_EQUAL_STRING("x", d.text());
}

void test_ctrl_backspace_is_one_undo_step() {
  Document d = makeDoc();
  type(d, "Hallo Welt");
  d.breakUndoGroup();
  d.deleteWordBefore();
  TEST_ASSERT_EQUAL_STRING("Hallo ", d.text());
  TEST_ASSERT_TRUE(d.undo());
  TEST_ASSERT_EQUAL_STRING("Hallo Welt", d.text());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_empty);
  RUN_TEST(test_insert_advances_the_cursor);
  RUN_TEST(test_insert_happens_at_the_cursor_not_the_end);
  RUN_TEST(test_cursor_snaps_to_character_boundaries);
  RUN_TEST(test_arrow_keys_step_by_character_not_byte);
  RUN_TEST(test_backspace_removes_a_whole_character);
  RUN_TEST(test_backspace_at_the_start_is_a_no_op);
  RUN_TEST(test_delete_forward_removes_a_whole_character);
  RUN_TEST(test_line_start_and_end);
  RUN_TEST(test_word_movement_treats_umlauts_as_word_characters);
  RUN_TEST(test_capacity_is_respected);
  RUN_TEST(test_undo_of_a_typing_run_removes_the_whole_word);
  RUN_TEST(test_undo_steps_word_by_word);
  RUN_TEST(test_undo_restores_deleted_text);
  RUN_TEST(test_undo_restores_a_deleted_umlaut_intact);
  RUN_TEST(test_cursor_movement_breaks_the_undo_group);
  RUN_TEST(test_undo_on_an_empty_history_is_a_no_op);
  RUN_TEST(test_document_without_undo_storage_still_edits);
  RUN_TEST(test_dirty_flag_tracks_edits);
  RUN_TEST(test_ctrl_backspace_deletes_a_whole_word);
  RUN_TEST(test_ctrl_backspace_eats_trailing_space_with_the_word);
  RUN_TEST(test_ctrl_backspace_keeps_umlauts_whole);
  RUN_TEST(test_ctrl_backspace_at_the_start_is_a_no_op);
  RUN_TEST(test_ctrl_backspace_is_one_undo_step);
  return UNITY_END();
}
