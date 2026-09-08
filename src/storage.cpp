#include "storage.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <string.h>

#include <Rtc.h>

#include "core/project.h"
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

bool Storage::openBook(const char* bookTitle) {
  if (!mounted_) return fail("not mounted");
  slugify(bookTitle, book_, sizeof(book_), "buch");

  auto& sd = SDCardManager::getInstance();
  char dir[96];
  if (!sd.exists(kBooks) && !sd.mkdir(kBooks)) return fail("cannot create /books");
  snprintf(dir, sizeof(dir), "%s/%s", kBooks, book_);
  if (!sd.exists(dir) && !sd.mkdir(dir)) return fail("cannot create book directory");
  snprintf(dir, sizeof(dir), "%s/%s/chapters", kBooks, book_);
  if (!sd.exists(dir) && !sd.mkdir(dir)) return fail("cannot create chapters directory");

  Serial.printf("[book] %s ready\n", dir);
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
