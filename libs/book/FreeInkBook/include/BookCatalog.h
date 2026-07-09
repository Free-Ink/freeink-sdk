#pragma once

// FreeInk SDK -- SD-backed container catalog for FreeInkBook.
//
// Book keeps a whole container's metadata (ZIP names, manifest, spine, TOC)
// in the book arena; webnovel omnibuses (1,700+ spine items) need ~400 KB of
// it, which no small target can hold. BookCatalog trades that for a one-time
// index build: build() streams the ZIP central directory, the OPF package,
// and the TOC into a compact index file ("catalog.fibc") in the book's cache
// directory, and open() keeps only fixed-size tables resident:
//
//   resident   spine records (16 B each: name-pool offset, local header
//              offset, uncompressed size, method, entry index) + a
//              hash-sorted entry table (8 B each: name hash, spine index)
//              + the metadata strings + resolved CSS/cover entries
//              -- ~44 KB for a 1,732-spine omnibus
//   on SD      spine/manifest name pool, TOC records + title pool, full
//              entry records (16 B, random access by index)
//
// The file is keyed by a fingerprint of the ZIP central directory plus the
// catalog format version; open() returns Stale when the container changed
// so the caller can rebuild.
//
// Name lookups (zip().find / findByHash, spineIndexForHref) match on the
// FNV-1a name hash alone -- with one book's worth of entries a 32-bit
// collision is vanishingly unlikely, and the strings it would take to verify
// are exactly what this class exists to evict from RAM.
//
// File layout (little-endian, offsets from file start, footer fixed-size at
// the very end so the writer streams every section without rewinding):
//   namePool     manifest hrefs, NUL-terminated
//   titlePool    TOC titles + fragments, NUL-terminated
//   entryRecords entryCount x 16 B {u32 localHeaderOffset, u32 compressedSize,
//                u32 uncompressedSize, u16 method, u16 reserved}, hash-sorted
//   spineRecords spineCount x 16 B {u32 nameOffset, u32 localHeaderOffset,
//                u32 uncompressedSize, u16 method, u16 entryIndex}
//   tocRecords   tocCount x 12 B {u32 titleOffset, u32 fragmentOffset,
//                u16 spineIndex, u8 depth, u8 reserved}
//   hashTable    entryCount x 8 B {u32 nameHash, u16 spineIndex, u16 reserved}
//                (row i describes entryRecords[i]; rows are hash-sorted)
//   meta         title\0 author\0 language\0 identifier\0 + cssCount x u16
//   cssRules     the compacted stylesheet: ruleCount x sizeof(CssRule) POD
//                rows (parsed from every CSS entry at build time)
//   footer       section offsets/counts + fingerprint + 'F''I''B''C'

#include <stddef.h>
#include <stdint.h>

#include "BookArena.h"
#include "BookStorage.h"
#include "BookTypes.h"
#include "css/Css.h"
#include "epub/ZipCatalog.h"

namespace freeink {
namespace book {

class BookCatalog {
 public:
  static constexpr const char* kCatalogName = "catalog.fibc";

  // Builds catalog.fibc from the container. `scratch` holds the fixed-size
  // record tables (~70 KB peak for a 1,732-spine omnibus); `parseArena`, when
  // given, takes the streaming parse state (inflate window + XML buffers,
  // ~46 KB for deflated package documents) so neither arena needs to cover
  // both -- the same split ChapterLayout::layout uses. Without it everything
  // comes from `scratch`.
  static BookStatus build(BookSource& source, CacheStorage& cache, Arena& scratch,
                          Arena* parseArena = nullptr);

  // Bytes of arena open() will consume, from the footer -- for exact sizing.
  static BookStatus residentBytes(CacheStorage& cache, size_t* out);

  // Validates the footer and the container fingerprint (Stale when the ZIP
  // changed or the format version moved) and loads the resident tables.
  // `source` and `cache` must outlive the catalog; the arena must stay live
  // and unreset while the catalog is used. `scratch` covers the fingerprint
  // scan buffer only (~2 KB, released before returning).
  BookStatus open(BookSource& source, CacheStorage& cache, Arena& bookArena, Arena& scratch);

  bool isOpen() const { return cache_ != nullptr; }

  const BookMetadata& metadata() const { return metadata_; }
  size_t spineCount() const { return spineCount_; }
  size_t tocCount() const { return tocCount_; }

  // Uncompressed bytes of a spine item (resident) -- whole-book progress math.
  uint32_t spineSize(size_t index) const;

  // Full ZipEntry for a spine item (one 16-byte index read; out->name is "").
  BookStatus spineEntry(size_t index, ZipEntry* out) const;

  // Container path of a spine item (one name-pool read).
  BookStatus spineHref(size_t index, char* buf, size_t cap) const;

  // Spine index for a container href, -1 when the href is not a spine item.
  int spineIndexForHref(const char* href) const;

  // TOC access. Title/fragment strings are copied into the caller's buffers
  // (truncated to fit); out->spineIndex is -1 when the entry's target is not
  // a spine item, out->hasFragment false when there is no anchor.
  struct TocItem {
    int spineIndex = -1;
    uint8_t depth = 0;
    bool hasFragment = false;
  };
  BookStatus tocItem(size_t index, TocItem* out, char* titleBuf, size_t titleCap,
                     char* fragmentBuf, size_t fragmentCap) const;

  // Last TOC entry at or before `spineIndex` (the chapter's display title);
  // -1 when the TOC has no such entry. Scans the fixed-size records in
  // chunks -- no title strings are read.
  int tocIndexForSpine(int spineIndex) const;

  // Stylesheet items (text/css manifest entries), resolved at open().
  size_t cssCount() const { return cssCount_; }
  const ZipEntry* cssEntry(size_t index) const {
    return index < cssCount_ ? &cssEntries_[index] : nullptr;
  }

  // The book's compacted stylesheet, parsed once at build() from every CSS
  // entry and stored in the catalog file. open() loads the rule array resident
  // in the book arena, so the caller never re-parses CSS from the container --
  // byte-identical every open, which keeps the layout generation hash stable.
  // nullptr when the catalog was built without any parsable CSS.
  const CssStylesheet* stylesheet() const { return sheet_.ruleCount > 0 ? &sheet_ : nullptr; }

  // The container's cover image, when the package declares one.
  bool coverEntry(ZipEntry* out) const;

  // ZipCatalog-compatible lookup view for layout and image rendering: find()
  // and findByHash() binary-search the resident hash table and read the
  // 16-byte entry record from SD. The returned pointer aims at a single
  // internal slot -- it stays valid until the NEXT find call, which every
  // engine consumer (probe, image decode) satisfies today.
  const ZipCatalog& zip() const { return zipView_; }

 private:
  friend class CatalogZipView;

  struct SpineRec {
    uint32_t nameOffset;
    uint32_t localHeaderOffset;
    uint32_t uncompressedSize;
    uint16_t method;
    uint16_t entryIndex;
  };
  struct HashRec {
    uint32_t nameHash;
    uint16_t spineIndex;  // 0xFFFF = not a spine item
    uint16_t reserved;
  };
  static_assert(sizeof(SpineRec) == 16, "catalog spine record layout");
  static_assert(sizeof(HashRec) == 8, "catalog hash record layout");

  class CatalogZipView : public ZipCatalog {
   public:
    const ZipEntry* find(const char* path) const override;
    const ZipEntry* findByHash(uint32_t nameHash) const override;
    BookCatalog* owner_ = nullptr;
    mutable ZipEntry slot_{};
  };

  // -1 when the hash is absent; otherwise the hash-table row (== entry index).
  int hashRowFor(uint32_t nameHash) const;
  BookStatus readEntryRecord(uint32_t entryIndex, ZipEntry* out) const;
  BookStatus readPoolString(uint32_t poolOffset, uint32_t stringOffset, char* buf,
                            size_t cap) const;

  CacheStorage* cache_ = nullptr;
  BookMetadata metadata_;
  const SpineRec* spineRecs_ = nullptr;
  const HashRec* hashRecs_ = nullptr;
  ZipEntry* cssEntries_ = nullptr;
  CssStylesheet sheet_;  // rules resident in the book arena (empty when none)
  uint32_t spineCount_ = 0;
  uint32_t tocCount_ = 0;
  uint32_t entryCount_ = 0;
  uint32_t cssCount_ = 0;
  uint32_t namePoolOff_ = 0;
  uint32_t titlePoolOff_ = 0;
  uint32_t entryRecordsOff_ = 0;
  uint32_t tocRecordsOff_ = 0;
  bool hasCover_ = false;
  ZipEntry coverEntry_{};
  CatalogZipView zipView_;
};

}  // namespace book
}  // namespace freeink
