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
  // The index (8 bytes per page) accumulates in `arena` in chunks allocated
  // as pages arrive, until finish(). `arena` must stay valid throughout.
  bool begin(CacheStorage& storage, const char* name, uint32_t generationHash, Arena& arena);
  bool onPage(const Page& page) override;
  void onAnchor(uint32_t idHash, uint32_t charStart) override;
  bool finish();
  // Set before finish(): total extracted characters in the chapter — the
  // denominator for reading-percentage (kosync-style progress).
  void setTotalChars(uint32_t totalChars) { totalChars_ = totalChars; }
  bool failed() const { return failed_; }
  uint32_t pageCount() const { return pageCount_; }

  // --- Mid-build access (incremental layout) -------------------------------
  // While a chapter is still building, the writer can serve the pages it has
  // already written: the index lives in its chunk list, and the blob bytes
  // come back through CacheStorage::readBackAt(). This is what lets the
  // reader UI show page N while layout continues past it.
  BookStatus readPage(uint32_t pageIndex, Arena& scratch, Page* out);
  uint32_t charStart(uint32_t pageIndex) const;
  // Last written page whose charStart <= charOffset (0 when none yet).
  uint32_t pageForChar(uint32_t charOffset) const;
  bool charForAnchor(uint32_t idHash, uint32_t* charOut) const;

  // Commits the build-so-far as a PARTIAL cache file: index + anchors + a
  // partial footer carrying `bytesConsumed`/`bytesTotal` (input-side build
  // progress, for estimated-total display) and the chars extracted so far.
  // PageCacheReader::open() accepts the partial (isPartial() true) so a
  // reopened book serves these pages instantly while a fresh background
  // build re-lays the rest. The writer is closed afterwards.
  bool suspend(uint32_t bytesConsumed, uint32_t bytesTotal);

 private:
  // The page index accumulates as a chunk list allocated on demand, so a
  // typical 10-30 page chapter costs one ~1 KB chunk instead of the full
  // kMaxPages reservation (which on the small tier is 8 KB the host cannot
  // spare during a build). kMaxPages stays as the hard sanity cap only.
  struct IndexChunk {
    static constexpr uint32_t kEntries = 128;
    uint32_t offsets[kEntries];
    uint32_t charStarts[kEntries];
    IndexChunk* next;
  };

  bool writeRaw(const void* data, uint32_t len);
  const IndexChunk* chunkFor(uint32_t pageIndex) const;
  // Streams the page index + anchor table; shared by finish() and suspend().
  bool writeIndexAndAnchors(uint32_t* indexOffsetOut, uint32_t* anchorOffsetOut);

  // Profile-tiered like the layout capacities (BookProfile.h): the small
  // tier trades pathological single-spine books for a smaller anchor table.
#if FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_SMALL
  // With the chunked index the cap costs nothing up front — the build arena
  // is the real limit (~8 B/page as chunks). Whole-novel single-spine files
  // exceed 1024 pages routinely (observed: 1174), so the cap matches the
  // standard tier and exists only as a runaway backstop.
  static constexpr uint32_t kMaxPages = 4096;
  static constexpr uint32_t kMaxAnchors = 192;
#elif FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_LARGE
  static constexpr uint32_t kMaxPages = 8192;
  static constexpr uint32_t kMaxAnchors = 1024;
#else
  static constexpr uint32_t kMaxPages = 4096;
  static constexpr uint32_t kMaxAnchors = 512;
#endif

  CacheStorage* storage_ = nullptr;
  const char* name_ = nullptr;
  Arena* arena_ = nullptr;  // chunk source; caller's build scratch
  IndexChunk* firstChunk_ = nullptr;
  IndexChunk* curChunk_ = nullptr;
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

  // True when the file is a suspended partial build (see
  // PageCacheWriter::suspend): pageCount() is a watermark, not the chapter
  // total, and totalChars() covers only the built prefix. Callers serve
  // these pages immediately and rebuild the rest in the background.
  bool isPartial() const { return isPartial_; }
  // Input-side progress the partial was suspended at — the basis for an
  // estimated total page count (pageCount / consumed * total). 0 on final
  // caches.
  uint32_t buildBytesConsumed() const { return buildBytesConsumed_; }
  uint32_t buildBytesTotal() const { return buildBytesTotal_; }

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
  uint32_t buildBytesConsumed_ = 0;
  uint32_t buildBytesTotal_ = 0;
  bool isPartial_ = false;
};

}  // namespace book
}  // namespace freeink
