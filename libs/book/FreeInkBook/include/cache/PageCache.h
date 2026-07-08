#pragma once

// FreeInk SDK — page cache for FreeInkBook (Phase 3).
//
// Layout runs once; pages are serialized to compact binary records in the
// application's cache storage. Turning a page is then one index lookup and
// one sequential read — no ZIP, no inflate, no XML, no layout. A cache file
// is keyed by a generation hash of everything that affects layout (page
// geometry, base font size, font identity, engine version); changing the
// font size simply produces a different generation, and the page anchors
// (chapter character offsets) carry the reading position across generations
// exactly.
//
// File format (little-endian, all fields packed):
//   header : 'F''I''B''P' u16 version u16 reserved u32 generationHash
//   blobs  : per page — u32 charStart, u16 runCount, u16 reserved, then runs
//            {i16 x, i16 baselineY, u16 sizePx, u8 flags, u8 reserved,
//             u16 textLen, bytes}
//   index  : per page — u32 blobOffset, u32 charStart
//   footer : u32 indexOffset, u32 pageCount, 'F''I''B''X'
// The index and footer live at the end so the writer streams blobs without
// knowing the page count up front.

#include <stdint.h>

#include "BookProfile.h"

#include "BookArena.h"
#include "BookStorage.h"
#include "BookTypes.h"
#include "layout/ChapterLayout.h"

namespace freeink {
namespace book {

// Hash of everything that must invalidate cached layout when it changes.
// `fontFingerprint` identifies the font set (bump it when registered fonts
// change); the engine's layout version is mixed in automatically.
uint32_t layoutGenerationHash(const LayoutParams& params, uint32_t fontFingerprint);

// Builds "s<spineIndex>-<hash8>.fibp" into `out`. Returns false if it does
// not fit.
bool pageCacheName(uint16_t spineIndex, uint32_t generationHash, char* out, uint32_t outCap);

// PageSink that serializes every delivered page to cache storage. Feed it to
// ChapterLayout::layout(), then call finish(). On any storage failure the
// writer aborts the file (removes the partial) and reports failed().
class PageCacheWriter : public PageSink {
 public:
  // The index (8 bytes per page) accumulates in `arena` until finish().
  bool begin(CacheStorage& storage, const char* name, uint32_t generationHash, Arena& arena);
  bool onPage(const Page& page) override;
  void onAnchor(uint32_t idHash, uint32_t charStart) override;
  bool finish();
  // Set before finish(): total extracted characters in the chapter — the
  // denominator for reading-percentage (kosync-style progress).
  void setTotalChars(uint32_t totalChars) { totalChars_ = totalChars; }
  bool failed() const { return failed_; }
  uint32_t pageCount() const { return pageCount_; }

 private:
  bool writeRaw(const void* data, uint32_t len);

  // Index arrays (8 bytes per page + 8 per anchor) accumulate in the caller's
  // scratch arena for the whole pagination — the dominant fixed cost of a
  // build after the layout buffers. Profile-tiered like the layout capacities
  // (BookProfile.h): the small tier trades pathological single-spine books
  // (>2048 pages at its buffer sizes) for ~20 KB less build scratch.
#if FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_SMALL
  static constexpr uint32_t kMaxPages = 2048;
  static constexpr uint32_t kMaxAnchors = 256;
#elif FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_LARGE
  static constexpr uint32_t kMaxPages = 8192;
  static constexpr uint32_t kMaxAnchors = 1024;
#else
  static constexpr uint32_t kMaxPages = 4096;
  static constexpr uint32_t kMaxAnchors = 512;
#endif

  CacheStorage* storage_ = nullptr;
  const char* name_ = nullptr;
  uint32_t* offsets_ = nullptr;
  uint32_t* charStarts_ = nullptr;
  uint32_t* anchorHashes_ = nullptr;
  uint32_t* anchorChars_ = nullptr;
  uint32_t anchorCount_ = 0;
  uint32_t totalChars_ = 0;
  uint32_t pageCount_ = 0;
  uint32_t writeOffset_ = 0;
  bool failed_ = false;
  bool open_ = false;
};

// Random access to a cached chapter. open() validates the generation hash;
// a mismatch returns BookStatus::Stale so the caller can rebuild.
class PageCacheReader {
 public:
  // Index data (8 bytes per page) and a copy of `name` are read into `arena`
  // and stay valid until that arena resets.
  BookStatus open(CacheStorage& storage, const char* name, uint32_t expectedHash, Arena& arena);

  uint32_t pageCount() const { return pageCount_; }
  uint32_t charStart(uint32_t pageIndex) const {
    return pageIndex < pageCount_ ? charStarts_[pageIndex] : 0;
  }

  // The page containing `charOffset` — the position-restore primitive.
  uint32_t pageForChar(uint32_t charOffset) const;

  // Total extracted characters in the chapter (percentage denominator).
  uint32_t totalChars() const { return totalChars_; }

  // Resolves an id="" anchor (FNV hash via ZipCatalog::hashPath) to its
  // chapter character offset — link/footnote jumps: charForAnchor →
  // pageForChar. False when the chapter has no such id.
  bool charForAnchor(uint32_t idHash, uint32_t* charOut) const;

  // Decodes one page. Run records and text are allocated from `scratch`;
  // take a mark first and release after rendering. The returned Page points
  // into that allocation.
  BookStatus readPage(uint32_t pageIndex, Arena& scratch, Page* out);

 private:
  CacheStorage* storage_ = nullptr;
  const char* name_ = nullptr;  // arena-owned copy (made in open())
  uint32_t* offsets_ = nullptr;
  uint32_t* charStarts_ = nullptr;
  uint32_t* anchorHashes_ = nullptr;
  uint32_t* anchorChars_ = nullptr;
  uint32_t anchorCount_ = 0;
  uint32_t totalChars_ = 0;
  uint32_t pageCount_ = 0;
  uint32_t indexOffset_ = 0;
};

}  // namespace book
}  // namespace freeink
