#pragma once

// Turn a human title into a safe ASCII filename.
//
// Not cosmetic: the SDK's own header warns that "SdFat mangles any non-ASCII
// long filename into an unopenable path". A chapter called "Größe" must not
// become a file nobody can reopen -- including us, on the next boot.
//
// Transliterates rather than strips, so German titles stay readable:
// "Der Größe nach" -> "der-groesse-nach".

#include <stdint.h>
#include <stddef.h>

namespace pocketx {

// Writes a NUL-terminated slug into `out`. Returns its length.
// Never produces an empty name: an untitled or unusable input yields `fallback`.
size_t slugify(const char* utf8Title, char* out, size_t outSize, const char* fallback = "unbenannt");

}  // namespace pocketx
