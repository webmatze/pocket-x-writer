#include "storage.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <string.h>

#include "core/slug.h"

namespace pocketx {
namespace {
constexpr const char* kDir = "/notes";
constexpr uint32_t kChunk = 512;
}  // namespace

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
