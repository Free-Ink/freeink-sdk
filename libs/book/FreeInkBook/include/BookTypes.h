#pragma once

// FreeInk SDK — shared value types for FreeInkBook.
//
// All strings are NUL-terminated UTF-8 owned by the book arena; they stay
// valid until the arena that backed Book::open() is reset.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace book {

enum class BookStatus : uint8_t {
  Ok = 0,
  IoError,       // BookSource read failed
  NotZip,        // no ZIP structure found
  NotEpub,       // ZIP without EPUB container/package
  Encrypted,     // DRM (META-INF/encryption.xml present) — unsupported
  Truncated,     // ZIP structure points past end of file
  Unsupported,   // valid but out of scope (zip64, unknown compression)
  OutOfMemory,   // an arena was exhausted
  ParseError,    // malformed XML in container/package/TOC
  NotFound,      // named item missing from the container
  Stale,         // cache generation does not match — rebuild needed
};

// Human-readable name for logs and tests.
const char* bookStatusName(BookStatus status);

struct BookMetadata {
  const char* title = "";
  const char* author = "";
  const char* language = "";
  const char* identifier = "";
};

struct ManifestItem {
  const char* id = nullptr;
  const char* href = nullptr;       // resolved path inside the container
  const char* mediaType = nullptr;
  uint32_t idHash = 0;
  bool isNav = false;               // EPUB 3 navigation document
};

struct TocEntry {
  const char* title = "";
  const char* href = "";            // resolved path inside the container
  const char* fragment = nullptr;   // anchor within the target, or nullptr
  uint8_t depth = 0;                // 0 = top level
};

}  // namespace book
}  // namespace freeink
