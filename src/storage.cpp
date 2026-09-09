#include "storage.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <stdlib.h>
#include <string.h>

#include <Rtc.h>

#include "core/project.h"
#include "core/frontmatter.h"
#include "core/slug.h"

namespace pocketx {
namespace {
constexpr const char* kDir = "/notes";
constexpr uint32_t kChunk = 512;
}  // namespace

namespace {

freeink::Rtc* g_clock = nullptr;

// SdFat asks for the current time whenever it creates or updates a file.
void sdDateTime(uint16_t* date, uint16_t* time) {
  freeink::Rtc::DateTime t;
  if (!g_clock || !g_clock->now(t)) return;   // leave SdFat's default alone
  *date = FS_DATE(t.year, t.month, t.day);
  *time = FS_TIME(t.hour, t.minute, t.second);
}

}  // namespace

void Storage::useRtcForTimestamps() {
  static freeink::Rtc clock;
  if (!clock.begin()) {
    Serial.println("[sd] no RTC: file timestamps stay at the FAT epoch");
    return;
  }
  g_clock = &clock;
  FsDateTime::setCallback(sdDateTime);
  Serial.println("[sd] file timestamps now come from the RTC");
}

void Storage::pathFor(const char* title, char* out, uint32_t outSize, const char* ext) {
  char slug[48];
  slugify(title, slug, sizeof(slug));
  snprintf(out, outSize, "%s/%s%s", kDir, slug, ext);
}

bool Storage::fail(const char* msg) {
  err_ = msg;
  Serial.printf("[sd] error: %s\n", msg);
  return false;
}

bool Storage::begin() {
  auto& sd = SDCardManager::getInstance();
  mounted_ = sd.begin();
  if (!mounted_) return fail("card not mounted");
  if (!sd.exists(kDir) && !sd.mkdir(kDir)) return fail("cannot create /notes");
  Serial.println("[sd] mounted, /notes ready");
  return true;
}

bool Storage::exists(const char* title) {
  if (!mounted_) return false;
  char path[96];
  pathFor(title, path, sizeof(path));
  return SDCardManager::getInstance().exists(path);
}

bool Storage::save(const Document& doc, const char* title) {
  if (!mounted_) return fail("not mounted");

  auto& sd = SDCardManager::getInstance();
  char path[96], tmp[96], bak[96];
  pathFor(title, path, sizeof(path));
  pathFor(title, tmp, sizeof(tmp), ".tmp");
  pathFor(title, bak, sizeof(bak), ".bak");

  const char* text = doc.text();
  const uint32_t len = doc.size();

  // 1. Write the temp file.
  if (sd.exists(tmp)) sd.remove(tmp);
  {
    FsFile f = sd.open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return fail("cannot open temp file");
    uint32_t written = 0;
    while (written < len) {
      const uint32_t n = (len - written) < kChunk ? (len - written) : kChunk;
      if (f.write((const uint8_t*)text + written, n) != (int)n) {
        f.close();
        sd.remove(tmp);
        return fail("short write");
      }
      written += n;
    }
    f.sync();
    f.close();
  }

  // 2. Read it back and compare. A write that reported success but landed
  //    wrong is exactly the failure a backup cannot help with, so catch it here
  //    while the previous version is still untouched.
  {
    FsFile f = sd.open(tmp, O_RDONLY);
    if (!f) { sd.remove(tmp); return fail("cannot reopen temp file"); }
    if ((uint32_t)f.fileSize() != len) {
      f.close(); sd.remove(tmp);
      return fail("verify: size mismatch");
    }
    uint8_t buf[kChunk];
    uint32_t off = 0;
    while (off < len) {
      const uint32_t n = (len - off) < kChunk ? (len - off) : kChunk;
      if ((uint32_t)f.read(buf, n) != n || memcmp(buf, text + off, n) != 0) {
        f.close(); sd.remove(tmp);
        return fail("verify: content mismatch");
      }
      off += n;
    }
    f.close();
  }

  // 3. Rotate the previous version aside, then swap the verified file in.
  if (sd.exists(path)) {
    if (sd.exists(bak)) sd.remove(bak);
    if (!sd.rename(path, bak)) { sd.remove(tmp); return fail("cannot rotate backup"); }
  }
  if (!sd.rename(tmp, path)) {
    // Put the previous version back rather than leaving nothing in place.
    if (sd.exists(bak)) sd.rename(bak, path);
    return fail("cannot move temp into place");
  }

  Serial.printf("[sd] saved %s (%lu bytes, verified)\n", path, (unsigned long)len);
  return true;
}

bool Storage::load(Document& doc, const char* title) {
  if (!mounted_) return fail("not mounted");
  auto& sd = SDCardManager::getInstance();

  char path[96];
  pathFor(title, path, sizeof(path));
  if (!sd.exists(path)) { err_ = "no such file"; return false; }

  FsFile f = sd.open(path, O_RDONLY);
  if (!f) return fail("cannot open file");

  const uint32_t size = (uint32_t)f.fileSize();
  if (size + 1 > doc.capacity()) { f.close(); return fail("file larger than the document buffer"); }

  doc.clear();
  char buf[kChunk + 1];
  uint32_t off = 0;
  while (off < size) {
    const uint32_t n = (size - off) < kChunk ? (size - off) : kChunk;
    const int got = f.read((uint8_t*)buf, n);
    if (got <= 0) { f.close(); return fail("read failed"); }
    if (!doc.insert(buf, (uint32_t)got)) { f.close(); return fail("document rejected the content"); }
    off += (uint32_t)got;
  }
  f.close();

  doc.moveToEnd();
  doc.breakUndoGroup();
  doc.markClean();
  Serial.printf("[sd] loaded %s (%lu bytes)\n", path, (unsigned long)size);
  return true;
}

}  // namespace pocketx

namespace pocketx {
namespace {
constexpr const char* kBooks = "/books";
}  // namespace

namespace {

// Read a book's metadata file. Only the head is read: book.md may carry free
// notes below the block, and none of them are ours.
bool readBookMeta(const char* slug, char* out, uint32_t outSize) {
  auto& sd = SDCardManager::getInstance();
  char path[128];
  snprintf(path, sizeof(path), "%s/%s/book.md", kBooks, slug);
  if (!sd.exists(path)) return false;
  FsFile f = sd.open(path, O_RDONLY);
  if (!f) return false;
  const int got = f.read((uint8_t*)out, outSize - 1);
  f.close();
  if (got <= 0) return false;
  out[got] = 0;
  return true;
}

// Write a minimal metadata block. Newlines are stripped from the title: one of
// them inside the frontmatter would turn the rest of the file into something
// else entirely.
void writeBookMeta(const char* slug, const char* title) {
  char clean[64];
  uint32_t w = 0;
  for (const char* p = title; *p && w + 1 < sizeof(clean); ++p)
    if (*p != '\n' && *p != '\r') clean[w++] = *p;
  clean[w] = 0;
  if (!w) return;

  char path[128];
  snprintf(path, sizeof(path), "%s/%s/book.md", kBooks, slug);
  FsFile f = SDCardManager::getInstance().open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    // Not fatal: a book without metadata still opens, it just shows its slug.
    Serial.println("[book] warning: could not write book.md");
    return;
  }
  char meta[192];
  const int len = snprintf(meta, sizeof(meta), "---\ntitle: %s\n---\n\n", clean);
  if (len > 0) f.write((const uint8_t*)meta, (size_t)len);
  f.sync();
  f.close();
}

// Title of the book in `slug`, falling back to the directory name. A book whose
// metadata is missing or damaged is still a book -- that rule is why this
// returns void rather than a success flag.
void bookTitleFor(const char* slug, char* out, uint32_t outSize) {
  char head[512];
  if (readBookMeta(slug, head, sizeof(head)) &&
      frontmatterValue(head, "title", out, outSize))
    return;
  snprintf(out, outSize, "%s", slug);
}

}  // namespace

bool Storage::openBookBySlug(const char* slug) {
  if (!mounted_) return fail("not mounted");
  if (!slug || !*slug) return fail("no book given");
  snprintf(book_, sizeof(book_), "%s", slug);

  auto& sd = SDCardManager::getInstance();
  char dir[96];
  if (!sd.exists(kBooks) && !sd.mkdir(kBooks)) return fail("cannot create /books");
  snprintf(dir, sizeof(dir), "%s/%s", kBooks, book_);
  if (!sd.exists(dir) && !sd.mkdir(dir)) return fail("cannot create book directory");
  snprintf(dir, sizeof(dir), "%s/%s/chapters", kBooks, book_);
  if (!sd.exists(dir) && !sd.mkdir(dir)) return fail("cannot create chapters directory");

  bookTitleFor(book_, title_, sizeof(title_));
  Serial.printf("[book] %s ready (%s)\n", dir, title_);
  return true;
}

bool Storage::openBook(const char* bookTitle) {
  char slug[48];
  slugify(bookTitle, slug, sizeof(slug), "buch");
  if (!openBookBySlug(slug)) return false;

  // The caller knew a real title. If the directory carries no metadata yet, give
  // it some -- that is the migration for books made before book.md existed,
  // which would otherwise show their slug forever with the umlauts and capitals
  // gone. An existing block is never overwritten: it may have been edited.
  char head[512];
  if (!readBookMeta(slug, head, sizeof(head))) {
    writeBookMeta(slug, bookTitle);
    bookTitleFor(book_, title_, sizeof(title_));
  }
  return true;
}

uint16_t Storage::listBooks(Book* out, uint16_t max) {
  if (!mounted_ || !out || !max) return 0;
  auto& sd = SDCardManager::getInstance();
  if (!sd.exists(kBooks)) return 0;

  FsFile dir = sd.open(kBooks, O_RDONLY);
  if (!dir) return 0;

  uint16_t n = 0;
  FsFile f;
  while (n < max && f.openNext(&dir, O_RDONLY)) {
    char name[64] = {0};
    f.getName(name, sizeof(name));
    const bool isDir = f.isDir();
    f.close();
    if (!isDir || name[0] == '.') continue;   // skip .Trashes and friends

    Book b;
    snprintf(b.slug, sizeof(b.slug), "%s", name);
    bookTitleFor(b.slug, b.title, sizeof(b.title));
    out[n++] = b;
  }
  dir.close();

  // Sort by title, so the list reads the way the writer thinks of it rather
  // than the way the card happens to hand the directories back.
  for (uint16_t i = 1; i < n; ++i)
    for (uint16_t j = i; j > 0 && strcasecmp(out[j].title, out[j - 1].title) < 0; --j) {
      const Book t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
    }
  return n;
}

bool Storage::rememberChapter(uint16_t number) {
  if (!mounted_ || !book_[0]) return false;
  auto& sd = SDCardManager::getInstance();
  char path[128], tmp[128], bak[128];
  snprintf(path, sizeof(path), "%s/%s/book.md", kBooks, book_);
  snprintf(tmp, sizeof(tmp), "%s/%s/book.tmp", kBooks, book_);
  snprintf(bak, sizeof(bak), "%s/%s/book.bak", kBooks, book_);

  // Static, not stack: together these are more than the Arduino task stack has
  // to spare, and this runs from the loop task.
  static char in[4096];
  static char out[4608];
  in[0] = 0;

  if (sd.exists(path)) {
    FsFile f = sd.open(path, O_RDONLY);
    if (!f) return false;
    const uint32_t size = (uint32_t)f.fileSize();
    if (size + 1 > sizeof(in)) {
      f.close();
      // Notes longer than the buffer. Leave the file completely alone: losing
      // someone's notes to remember a chapter number is a terrible trade.
      Serial.println("[book] book.md too large to update; place not remembered");
      return false;
    }
    const int got = f.read((uint8_t*)in, size);
    f.close();
    if (got < 0) return false;
    in[got] = 0;
  }

  char value[12];
  snprintf(value, sizeof(value), "%u", (unsigned)number);
  if (!frontmatterSet(in, "open", value, out, sizeof(out))) return false;

  // Same discipline as a chapter save: the previous version is only released
  // once the new one is safely on the card.
  if (sd.exists(tmp)) sd.remove(tmp);
  {
    FsFile f = sd.open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return false;
    const uint32_t len = (uint32_t)strlen(out);
    const bool ok = f.write((const uint8_t*)out, len) == (int)len;
    f.sync();
    f.close();
    if (!ok) { sd.remove(tmp); return false; }
  }
  if (sd.exists(path)) {
    if (sd.exists(bak)) sd.remove(bak);
    if (!sd.rename(path, bak)) { sd.remove(tmp); return false; }
  }
  if (!sd.rename(tmp, path)) {
    if (sd.exists(bak)) sd.rename(bak, path);
    return false;
  }
  return true;
}

uint16_t Storage::rememberedChapter() {
  if (!mounted_ || !book_[0]) return 0;
  char head[1024], v[12];
  if (!readBookMeta(book_, head, sizeof(head))) return 0;
  if (!frontmatterValue(head, "open", v, sizeof(v))) return 0;
  const int n = atoi(v);
  return n > 0 && n < 10000 ? (uint16_t)n : 0;
}

bool Storage::createBook(const char* title, Book* out) {
  if (!mounted_ || !out) return fail("not mounted");

  Book b;
  slugify(title, b.slug, sizeof(b.slug), "buch");

  auto& sd = SDCardManager::getInstance();
  char dir[96];
  if (!sd.exists(kBooks) && !sd.mkdir(kBooks)) return fail("cannot create /books");
  snprintf(dir, sizeof(dir), "%s/%s", kBooks, b.slug);
  if (sd.exists(dir)) return fail("a book of that name already exists");
  if (!sd.mkdir(dir)) return fail("cannot create book directory");

  char chapters[128];
  snprintf(chapters, sizeof(chapters), "%s/%s/chapters", kBooks, b.slug);
  if (!sd.mkdir(chapters)) return fail("cannot create chapters directory");

  // The title as typed, so the slug's lost umlauts and capitals survive.
  writeBookMeta(b.slug, title);
  bookTitleFor(b.slug, b.title, sizeof(b.title));

  *out = b;
  Serial.printf("[book] created %s (%s)\n", dir, b.title);
  return true;
}

void Storage::chapterPath(const Chapter& ch, char* out, uint32_t outSize, const char* ext) const {
  snprintf(out, outSize, "%s/%s/chapters/%s%s", kBooks, book_, ch.file, ext);
}

uint16_t Storage::listChapters(Chapter* out, uint16_t max) {
  if (!mounted_ || !out || !max) return 0;
  auto& sd = SDCardManager::getInstance();

  char dirPath[96];
  snprintf(dirPath, sizeof(dirPath), "%s/%s/chapters", kBooks, book_);
  FsFile dir = sd.open(dirPath, O_RDONLY);
  if (!dir) return 0;

  uint16_t n = 0;
  FsFile f;
  while (n < max && f.openNext(&dir, O_RDONLY)) {
    char name[64] = {0};
    f.getName(name, sizeof(name));
    const uint32_t size = (uint32_t)f.fileSize();
    const bool isDir = f.isDir();
    f.close();
    if (isDir) continue;

    Chapter c;
    if (!parseChapterFileName(name, &c.number, c.slug, sizeof(c.slug))) continue;
    strncpy(c.file, name, sizeof(c.file) - 1);
    c.bytes = size;
    out[n++] = c;
  }
  dir.close();

  // Sort by chapter number: the card hands files back in directory order, which
  // is not reading order.
  for (uint16_t i = 1; i < n; ++i)
    for (uint16_t j = i; j > 0 && out[j].number < out[j - 1].number; --j) {
      const Chapter t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
    }
  return n;
}

bool Storage::createChapter(const char* title, Chapter* out) {
  if (!mounted_ || !out) return fail("not mounted");

  Chapter existing[kMaxChapters];
  const uint16_t n = listChapters(existing, kMaxChapters);
  uint16_t next = 1;
  for (uint16_t i = 0; i < n; ++i)
    if (existing[i].number >= next) next = existing[i].number + 1;

  Chapter c;
  c.number = next;
  chapterFileName(next, title, c.file, sizeof(c.file));
  parseChapterFileName(c.file, nullptr, c.slug, sizeof(c.slug));
  c.bytes = 0;

  // Create it empty so it shows up in the list even before the first save.
  char path[160];
  chapterPath(c, path, sizeof(path));
  FsFile f = SDCardManager::getInstance().open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return fail("cannot create chapter file");
  f.close();

  *out = c;
  Serial.printf("[book] created %s\n", path);
  return true;
}

bool Storage::chapterHead(const Chapter& ch, char* out, uint32_t outSize) {
  if (!out || outSize == 0) return false;
  out[0] = 0;
  if (!mounted_) return false;
  auto& sd = SDCardManager::getInstance();
  char path[160];
  chapterPath(ch, path, sizeof(path));
  if (!sd.exists(path)) return false;

  FsFile f = sd.open(path, O_RDONLY);
  if (!f) return false;
  const int got = f.read((uint8_t*)out, outSize - 1);
  f.close();
  if (got <= 0) return false;
  out[got] = 0;
  return true;
}

bool Storage::loadChapter(Document& doc, const Chapter& ch) {
  if (!mounted_) return fail("not mounted");
  auto& sd = SDCardManager::getInstance();
  char path[160];
  chapterPath(ch, path, sizeof(path));

  doc.clear();
  if (!sd.exists(path)) { doc.markClean(); return true; }   // a new chapter is simply empty

  FsFile f = sd.open(path, O_RDONLY);
  if (!f) return fail("cannot open chapter");
  const uint32_t size = (uint32_t)f.fileSize();
  if (size + 1 > doc.capacity()) { f.close(); return fail("chapter larger than the buffer"); }

  char buf[512];
  uint32_t off = 0;
  while (off < size) {
    const uint32_t want = (size - off) < sizeof(buf) ? (size - off) : sizeof(buf);
    const int got = f.read((uint8_t*)buf, want);
    if (got <= 0) { f.close(); return fail("read failed"); }
    if (!doc.insert(buf, (uint32_t)got)) { f.close(); return fail("document rejected the content"); }
    off += (uint32_t)got;
  }
  f.close();
  doc.moveToStart();
  doc.breakUndoGroup();
  doc.markClean();
  Serial.printf("[book] loaded %s (%lu bytes)\n", path, (unsigned long)size);
  return true;
}

bool Storage::saveChapter(const Document& doc, const Chapter& ch) {
  if (!mounted_) return fail("not mounted");
  auto& sd = SDCardManager::getInstance();

  char path[160], tmp[160], bak[160];
  chapterPath(ch, path, sizeof(path));
  chapterPath(ch, tmp, sizeof(tmp), ".tmp");
  chapterPath(ch, bak, sizeof(bak), ".bak");

  const char* text = doc.text();
  const uint32_t len = doc.size();

  if (sd.exists(tmp)) sd.remove(tmp);
  {
    FsFile f = sd.open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return fail("cannot open temp file");
    uint32_t written = 0;
    while (written < len) {
      const uint32_t n = (len - written) < 512 ? (len - written) : 512;
      if (f.write((const uint8_t*)text + written, n) != (int)n) {
        f.close(); sd.remove(tmp);
        return fail("short write");
      }
      written += n;
    }
    f.sync();
    f.close();
  }
  {
    FsFile f = sd.open(tmp, O_RDONLY);
    if (!f) { sd.remove(tmp); return fail("cannot reopen temp file"); }
    if ((uint32_t)f.fileSize() != len) { f.close(); sd.remove(tmp); return fail("verify: size mismatch"); }
    uint8_t buf[512];
    uint32_t off = 0;
    while (off < len) {
      const uint32_t n = (len - off) < sizeof(buf) ? (len - off) : sizeof(buf);
      if ((uint32_t)f.read(buf, n) != n || memcmp(buf, text + off, n) != 0) {
        f.close(); sd.remove(tmp);
        return fail("verify: content mismatch");
      }
      off += n;
    }
    f.close();
  }
  if (sd.exists(path)) {
    if (sd.exists(bak)) sd.remove(bak);
    if (!sd.rename(path, bak)) { sd.remove(tmp); return fail("cannot rotate backup"); }
  }
  if (!sd.rename(tmp, path)) {
    if (sd.exists(bak)) sd.rename(bak, path);
    return fail("cannot move temp into place");
  }
  Serial.printf("[book] saved %s (%lu bytes, verified)\n", path, (unsigned long)len);
  return true;
}

}  // namespace pocketx
