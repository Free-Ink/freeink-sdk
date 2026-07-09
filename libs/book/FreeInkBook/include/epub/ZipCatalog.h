#pragma once

// FreeInk SDK — ZIP container access for FreeInkBook.
//
// ZipCatalog parses the central directory of an EPUB container into the book
// arena (entry names + fixed records; nothing else is retained). Item
// contents are then read through ZipEntryReader, which streams stored or
// deflated data in caller-sized chunks — an entry is never loaded whole.
//
// Out of scope for now (returns BookStatus::Unsupported): zip64 containers
// and compression methods other than stored/deflate. Books that need zip64
// exceed 4 GB and do not occur in practice.

#include <stddef.h>
#include <stdint.h>

#include "BookArena.h"
#include "BookStorage.h"
#include "BookTypes.h"

namespace freeink {
namespace book {

struct ZipEntry {
  const char* name = nullptr;  // path inside the container, arena-owned
  uint32_t nameHash = 0;       // FNV-1a of name
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint32_t localHeaderOffset = 0;
  uint16_t method = 0;         // 0 = stored, 8 = deflate
};

// find()/findByHash() are virtual so an SD-backed catalog (BookCatalog) can
// present the same lookup surface to layout and rendering without holding the
// in-RAM entry table this class builds.
class ZipCatalog {
 public:
  virtual ~ZipCatalog() = default;

  // Parses the central directory. Entry records and names are allocated from
  // `arena` and remain valid until that arena resets.
  BookStatus open(BookSource& source, Arena& arena);

  virtual const ZipEntry* find(const char* path) const;
  // Lookup by ZipCatalog::hashPath of the container path. Layout resolves
  // image/link targets through pre-hashed hrefs, so this is the hot form.
  virtual const ZipEntry* findByHash(uint32_t nameHash) const;
  size_t entryCount() const { return count_; }
  const ZipEntry* entry(size_t index) const {
    return index < count_ ? &entries_[index] : nullptr;
  }
  BookSource* source() const { return source_; }

  static uint32_t hashPath(const char* path);

  // Locates the end-of-central-directory record -- shared by open() and the
  // container fingerprint that keys the SD-backed catalog.
  static BookStatus locateCentralDirectory(BookSource& source, uint32_t* dirOffsetOut,
                                           uint32_t* dirSizeOut, uint16_t* entryCountOut);

 private:
  BookSource* source_ = nullptr;
  ZipEntry* entries_ = nullptr;
  size_t count_ = 0;
};

// Streaming reader for one ZIP entry. For deflated entries the inflate state
// (~11 KB decompressor + 32 KB window + 4 KB input buffer) is allocated from
// the scratch arena passed to open(); release the arena mark after the last
// read() to reclaim it. read() returns bytes produced, 0 at end of entry, or
// a negative value on error.
class ZipEntryReader {
 public:
  // Marks a synthetic entry as HEADERLESS: the source is the raw entry data
  // itself (offset 0), not a ZIP container. See rawEntry().
  static constexpr uint32_t kRawHeaderOffset = 0xFFFFFFFFu;

  // Synthesizes an entry describing a raw stored byte range — e.g. a chapter
  // previously extracted (inflated) to its own file so later layout passes
  // can skip the ~46 KB inflate state. open() reads it from offset 0 without
  // expecting a ZIP local header.
  static ZipEntry rawEntry(uint32_t size) {
    ZipEntry e;
    e.name = "";
    e.compressedSize = size;
    e.uncompressedSize = size;
    e.localHeaderOffset = kRawHeaderOffset;
    e.method = 0;  // stored
    return e;
  }

  BookStatus open(BookSource& source, const ZipEntry& entry, Arena& scratch);
  int32_t read(void* dst, uint32_t len);
  uint32_t totalProduced() const { return produced_; }
  const ZipEntry* entry() const { return entry_; }

 private:
  int32_t readStored(uint8_t* dst, uint32_t len);
  int32_t readDeflated(uint8_t* dst, uint32_t len);

  BookSource* source_ = nullptr;
  const ZipEntry* entry_ = nullptr;
  uint64_t dataOffset_ = 0;
  uint32_t produced_ = 0;

  // Deflate state (opaque here to keep miniz out of the public headers).
  void* decompressor_ = nullptr;
  uint8_t* window_ = nullptr;
  uint8_t* inBuf_ = nullptr;
  uint32_t inPos_ = 0;
  uint32_t inAvail_ = 0;
  uint32_t compConsumed_ = 0;
  uint32_t windowPos_ = 0;   // next write position in the 32 KB window
  uint32_t pendingPos_ = 0;  // start of decoded-but-undelivered bytes
  uint32_t pendingLen_ = 0;
  bool done_ = false;
};

}  // namespace book
}  // namespace freeink
