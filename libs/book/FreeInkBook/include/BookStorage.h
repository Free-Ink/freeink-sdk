#pragma once

// FreeInk SDK — storage interfaces for FreeInkBook.
//
// The engine never touches a filesystem directly. The application supplies a
// random-access view of the book container file (SD card, flash partition,
// or a host file in unit tests). Interfaces are deliberately tiny so a board
// adapter is a few lines.
//
// Freestanding C++17 — no Arduino or ESP-IDF dependency.

#include <stdint.h>

namespace freeink {
namespace book {

// Random-access byte source for one open book file.
//
// readAt() returns the number of bytes actually read (which may be short at
// end of file), or a negative value on I/O error. Implementations must allow
// arbitrary offsets; FreeInkBook seeks between the ZIP directory and item
// data while streaming.
class BookSource {
 public:
  virtual ~BookSource() = default;
  virtual int32_t readAt(uint64_t offset, void* dst, uint32_t len) = 0;
  virtual uint64_t size() const = 0;
};

// Writable store for one book's layout cache — flat file names inside a
// per-book directory the application owns. Writes are streaming (the engine
// never holds a whole cache file in RAM) with a single write open at a time;
// reads are random-access. A failed or interrupted write must leave either
// the old file or no file, never a torn one — implementations should write a
// temp name and rename on endWrite() where the filesystem allows.
class CacheStorage {
 public:
  virtual ~CacheStorage() = default;

  virtual bool exists(const char* name) = 0;
  virtual bool remove(const char* name) = 0;

  // Size of a cache file, or a negative value if it does not exist.
  virtual int64_t fileSize(const char* name) = 0;

  // Reads [offset, offset+len); returns bytes read or a negative value.
  virtual int32_t readAt(const char* name, uint32_t offset, void* dst, uint32_t len) = 0;

  // Streaming write of one file at a time.
  virtual bool beginWrite(const char* name) = 0;
  virtual bool write(const void* data, uint32_t len) = 0;
  virtual bool endWrite() = 0;
};

}  // namespace book
}  // namespace freeink
