// FreeInkBook — serialized page records: layout once, render many.

#include "cache/PageCache.h"

#include <stdio.h>
#include <string.h>

namespace freeink {
namespace book {

namespace {

constexpr uint16_t kFormatVersion = 3;  // v3: link records + anchor table
constexpr uint32_t kHeaderSize = 12;
constexpr uint32_t kFooterSize = 24;  // v3: + anchors + totalChars
// Partial (suspended-build) footer: the final footer's five u32 fields plus
// bytesConsumed + bytesTotal, sealed with "FIBx" instead of "FIBX". Old
// readers see a magic mismatch and treat the file as Stale — safe.
constexpr uint32_t kPartialFooterSize = 32;
constexpr uint32_t kMaxBlobSize = 128 * 1024;  // sanity bound on one page

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// CacheStorage::readAt may return short (SD adapters often pass one
// FsFile::read through); loop to the exact count.
bool readFully(CacheStorage& storage, const char* name, uint32_t off, void* dst, uint32_t len) {
  uint8_t* p = static_cast<uint8_t*>(dst);
  uint32_t got = 0;
  while (got < len) {
    const int32_t n = storage.readAt(name, off + got, p + got, len - got);
    if (n <= 0) return false;
    got += static_cast<uint32_t>(n);
  }
  return true;
}

uint32_t hashMix(uint32_t hash, uint32_t value) {
  hash ^= value;
  hash *= 16777619u;
  return hash;
}

}  // namespace

static BookStatus decodePageBlob(const uint8_t* blob, uint32_t blobLen, uint32_t pageIndex,
                                 Arena& scratch, Page* out);

// Bump when layout BEHAVIOR changes without a format change (ligatures,
// breaking rules, spacing math) — stale caches would otherwise render with
// mismatched widths after a firmware update.
constexpr uint32_t kLayoutRevision = 8;  // 8: inline CSS sizes/margins + line box sizing
                                         // (7: image dimension pre-scan,
                                         //  6: focus reading + non-ASCII hyphenation,
                                         //  5: Korean/CJ punct, 4: Arabic, 3: bidi, 2: ligatures)

uint32_t layoutGenerationHash(const LayoutParams& params, uint32_t fontFingerprint) {
  uint32_t hash = 2166136261u;
  hash = hashMix(hash, kFormatVersion);
  hash = hashMix(hash, kLayoutRevision);
  hash = hashMix(hash, static_cast<uint16_t>(params.pageWidth));
  hash = hashMix(hash, static_cast<uint16_t>(params.pageHeight));
  hash = hashMix(hash, static_cast<uint16_t>(params.marginLeft));
  hash = hashMix(hash, static_cast<uint16_t>(params.marginRight));
  hash = hashMix(hash, static_cast<uint16_t>(params.marginTop));
  hash = hashMix(hash, static_cast<uint16_t>(params.marginBottom));
  hash = hashMix(hash, params.baseSizePx);
  hash = hashMix(hash, fontFingerprint);
  hash = hashMix(hash, static_cast<uint32_t>(params.defaultAlign));
  hash = hashMix(hash, static_cast<uint32_t>(params.lineSpacingPct) << 16 | params.paragraphSpacingPct);
  hash = hashMix(hash, params.embeddedStyles ? 1u : 0u);
  hash = hashMix(hash, params.focusReading ? 1u : 0u);
  hash = hashMix(hash, static_cast<uint32_t>(params.orphanLines) << 8 | params.widowLines);
  hash = hashMix(hash, params.stylesheet != nullptr ? params.stylesheet->contentHash : 0);
  hash = hashMix(hash, params.hyphenator != nullptr && params.hyphenator->ready() ? 1u : 0u);
  for (const char* p = params.language; p != nullptr && *p != '\0'; ++p) {
    hash = hashMix(hash, static_cast<uint8_t>(*p));
  }
  return hash;
}

bool pageCacheName(uint16_t spineIndex, uint32_t generationHash, char* out, uint32_t outCap) {
  const int n = snprintf(out, outCap, "s%04u-%08x.fibp", spineIndex, generationHash);
  return n > 0 && static_cast<uint32_t>(n) < outCap;
}

// --- writer ------------------------------------------------------------------

bool PageCacheWriter::begin(CacheStorage& storage, const char* name, uint32_t generationHash,
                            Arena& arena) {
  storage_ = &storage;
  name_ = name;
  pageCount_ = 0;
  writeOffset_ = 0;
  failed_ = false;
  open_ = false;

  arena_ = &arena;
  firstChunk_ = nullptr;
  curChunk_ = nullptr;
  anchorHashes_ = arena.allocArray<uint32_t>(kMaxAnchors);
  anchorChars_ = arena.allocArray<uint32_t>(kMaxAnchors);
  anchorCount_ = 0;
  if (anchorHashes_ == nullptr || anchorChars_ == nullptr) {
    failed_ = true;
    return false;
  }
  if (!storage.beginWrite(name)) {
    failed_ = true;
    return false;
  }
  open_ = true;

  uint8_t header[kHeaderSize];
  header[0] = 'F';
  header[1] = 'I';
  header[2] = 'B';
  header[3] = 'P';
  putU16(header + 4, kFormatVersion);
  putU16(header + 6, 0);
  putU32(header + 8, generationHash);
  return writeRaw(header, sizeof(header));
}

bool PageCacheWriter::writeRaw(const void* data, uint32_t len) {
  if (failed_ || !open_) return false;
  if (!storage_->write(data, len)) {
    failed_ = true;
    return false;
  }
  writeOffset_ += len;
  return true;
}

void PageCacheWriter::onAnchor(uint32_t idHash, uint32_t charStart) {
  if (anchorCount_ < kMaxAnchors) {
    anchorHashes_[anchorCount_] = idHash;
    anchorChars_[anchorCount_] = charStart;
    ++anchorCount_;
  }
}

bool PageCacheWriter::onPage(const Page& page) {
  if (failed_) return false;
  if (pageCount_ >= kMaxPages) {
    failed_ = true;
    return false;
  }
  const uint32_t slot = pageCount_ % IndexChunk::kEntries;
  if (slot == 0) {  // current chunk full (or first page): grow the list
    IndexChunk* chunk = static_cast<IndexChunk*>(arena_->alloc(sizeof(IndexChunk), alignof(IndexChunk)));
    if (chunk == nullptr) {
      failed_ = true;
      return false;
    }
    chunk->next = nullptr;
    if (curChunk_ != nullptr) {
      curChunk_->next = chunk;
    } else {
      firstChunk_ = chunk;
    }
    curChunk_ = chunk;
  }
  curChunk_->offsets[slot] = writeOffset_;
  curChunk_->charStarts[slot] = page.charStart;
  ++pageCount_;

  uint8_t head[10];
  putU32(head, page.charStart);
  putU16(head + 4, page.runCount);
  putU16(head + 6, page.imageCount);
  putU16(head + 8, page.linkCount);
  if (!writeRaw(head, sizeof(head))) return false;

  for (uint16_t r = 0; r < page.runCount; ++r) {
    const PageTextRun& run = page.runs[r];
    uint8_t rec[10];
    putU16(rec, static_cast<uint16_t>(run.x));
    putU16(rec + 2, static_cast<uint16_t>(run.baselineY));
    putU16(rec + 4, run.sizePx);
    rec[6] = run.styleFlags;
    rec[7] = 0;
    putU16(rec + 8, run.len);
    if (!writeRaw(rec, sizeof(rec))) return false;
    if (!writeRaw(run.text, run.len)) return false;
  }
  for (uint16_t m = 0; m < page.imageCount; ++m) {
    const PageImage& img = page.images[m];
    const uint16_t hrefLen = static_cast<uint16_t>(strlen(img.href));
    uint8_t rec[10];
    putU16(rec, static_cast<uint16_t>(img.x));
    putU16(rec + 2, static_cast<uint16_t>(img.y));
    putU16(rec + 4, img.width);
    putU16(rec + 6, img.height);
    putU16(rec + 8, hrefLen);
    if (!writeRaw(rec, sizeof(rec))) return false;
    if (!writeRaw(img.href, hrefLen)) return false;
  }
  for (uint16_t l = 0; l < page.linkCount; ++l) {
    const PageLink& link = page.links[l];
    const uint16_t tLen = static_cast<uint16_t>(strlen(link.target));
    const uint16_t fLen = static_cast<uint16_t>(strlen(link.fragment));
    uint8_t rec[12];
    putU16(rec, static_cast<uint16_t>(link.x));
    putU16(rec + 2, static_cast<uint16_t>(link.y));
    putU16(rec + 4, link.width);
    putU16(rec + 6, link.height);
    putU16(rec + 8, tLen);
    putU16(rec + 10, fLen);
    if (!writeRaw(rec, sizeof(rec))) return false;
    if (!writeRaw(link.target, tLen)) return false;
    if (!writeRaw(link.fragment, fLen)) return false;
  }
  return true;
}

bool PageCacheWriter::finish() {
  if (failed_ || !open_) {
    if (open_) {
      storage_->endWrite();
      storage_->remove(name_);
      open_ = false;
    }
    return false;
  }

  uint32_t indexOffset = 0;
  uint32_t anchorOffset = 0;
  writeIndexAndAnchors(&indexOffset, &anchorOffset);
  uint8_t footer[kFooterSize];
  putU32(footer, indexOffset);
  putU32(footer + 4, pageCount_);
  putU32(footer + 8, anchorOffset);
  putU32(footer + 12, anchorCount_);
  putU32(footer + 16, totalChars_);
  footer[20] = 'F';
  footer[21] = 'I';
  footer[22] = 'B';
  footer[23] = 'X';
  writeRaw(footer, sizeof(footer));

  const bool committed = !failed_ && storage_->endWrite();
  open_ = false;
  if (!committed) {
    storage_->remove(name_);
    failed_ = true;
  }
  return !failed_;
}

bool PageCacheWriter::writeIndexAndAnchors(uint32_t* indexOffsetOut, uint32_t* anchorOffsetOut) {
  *indexOffsetOut = writeOffset_;
  const IndexChunk* chunk = firstChunk_;
  for (uint32_t i = 0; i < pageCount_ && !failed_; ++i) {
    const uint32_t slot = i % IndexChunk::kEntries;
    uint8_t rec[8];
    putU32(rec, chunk->offsets[slot]);
    putU32(rec + 4, chunk->charStarts[slot]);
    writeRaw(rec, sizeof(rec));
    if (slot == IndexChunk::kEntries - 1) chunk = chunk->next;
  }
  *anchorOffsetOut = writeOffset_;
  for (uint32_t a = 0; a < anchorCount_ && !failed_; ++a) {
    uint8_t rec[8];
    putU32(rec, anchorHashes_[a]);
    putU32(rec + 4, anchorChars_[a]);
    writeRaw(rec, sizeof(rec));
  }
  return !failed_;
}

bool PageCacheWriter::suspend(uint32_t bytesConsumed, uint32_t bytesTotal) {
  if (failed_ || !open_) {
    if (open_) {
      storage_->endWrite();
      storage_->remove(name_);
      open_ = false;
    }
    return false;
  }

  uint32_t indexOffset = 0;
  uint32_t anchorOffset = 0;
  writeIndexAndAnchors(&indexOffset, &anchorOffset);
  uint8_t footer[kPartialFooterSize];
  putU32(footer, indexOffset);
  putU32(footer + 4, pageCount_);
  putU32(footer + 8, anchorOffset);
  putU32(footer + 12, anchorCount_);
  putU32(footer + 16, totalChars_);
  putU32(footer + 20, bytesConsumed);
  putU32(footer + 24, bytesTotal);
  footer[28] = 'F';
  footer[29] = 'I';
  footer[30] = 'B';
  footer[31] = 'x';
  writeRaw(footer, sizeof(footer));

  const bool committed = !failed_ && storage_->endWrite();
  open_ = false;
  if (!committed) {
    storage_->remove(name_);
    failed_ = true;
  }
  return !failed_;
}

const PageCacheWriter::IndexChunk* PageCacheWriter::chunkFor(uint32_t pageIndex) const {
  const IndexChunk* chunk = firstChunk_;
  for (uint32_t hops = pageIndex / IndexChunk::kEntries; hops > 0 && chunk != nullptr; --hops) {
    chunk = chunk->next;
  }
  return chunk;
}

uint32_t PageCacheWriter::charStart(uint32_t pageIndex) const {
  if (pageIndex >= pageCount_) return 0;
  const IndexChunk* chunk = chunkFor(pageIndex);
  return chunk != nullptr ? chunk->charStarts[pageIndex % IndexChunk::kEntries] : 0;
}

uint32_t PageCacheWriter::pageForChar(uint32_t charOffset) const {
  if (pageCount_ == 0) return 0;
  // Linear over the chunk list — a giant chapter is ~6 chunks; this runs on
  // position restore, not per page turn.
  uint32_t best = 0;
  const IndexChunk* chunk = firstChunk_;
  for (uint32_t i = 0; i < pageCount_ && chunk != nullptr; ++i) {
    const uint32_t slot = i % IndexChunk::kEntries;
    if (chunk->charStarts[slot] <= charOffset) best = i;
    if (slot == IndexChunk::kEntries - 1) chunk = chunk->next;
  }
  return best;
}

bool PageCacheWriter::charForAnchor(uint32_t idHash, uint32_t* charOut) const {
  for (uint32_t a = 0; a < anchorCount_; ++a) {
    if (anchorHashes_[a] == idHash) {
      *charOut = anchorChars_[a];
      return true;
    }
  }
  return false;
}

BookStatus PageCacheWriter::readPage(uint32_t pageIndex, Arena& scratch, Page* out) {
  if (!open_ || failed_) return BookStatus::NotFound;
  if (pageIndex >= pageCount_) return BookStatus::NotFound;
  const IndexChunk* chunk = chunkFor(pageIndex);
  if (chunk == nullptr) return BookStatus::NotFound;
  const uint32_t blobOffset = chunk->offsets[pageIndex % IndexChunk::kEntries];
  uint32_t blobEnd = writeOffset_;
  if (pageIndex + 1 < pageCount_) {
    const IndexChunk* nextChunk =
        (pageIndex + 1) % IndexChunk::kEntries == 0 ? chunk->next : chunk;
    if (nextChunk == nullptr) return BookStatus::NotFound;
    blobEnd = nextChunk->offsets[(pageIndex + 1) % IndexChunk::kEntries];
  }
  if (blobEnd <= blobOffset || blobEnd - blobOffset > kMaxBlobSize) return BookStatus::Stale;
  const uint32_t blobLen = blobEnd - blobOffset;

  uint8_t* blob = static_cast<uint8_t*>(scratch.alloc(blobLen, 4));
  if (blob == nullptr) return BookStatus::OutOfMemory;
  uint32_t got = 0;
  while (got < blobLen) {
    const int32_t n = storage_->readBackAt(blobOffset + got, blob + got, blobLen - got);
    if (n <= 0) return BookStatus::IoError;  // readBackAt unsupported or short
    got += static_cast<uint32_t>(n);
  }
  return decodePageBlob(blob, blobLen, pageIndex, scratch, out);
}

// --- reader ------------------------------------------------------------------

BookStatus PageCacheReader::open(CacheStorage& storage, const char* name, uint32_t expectedHash,
                                 Arena& arena) {
  storage_ = &storage;
  // Own a copy: readPage() runs long after open(), and a borrowed stack
  // buffer dangling here reads the wrong file (structurally-valid index,
  // garbage blobs → Stale on every page).
  name_ = arena.strdup(name);
  pageCount_ = 0;
  if (name_ == nullptr) return BookStatus::OutOfMemory;

  const int64_t size = storage.fileSize(name);
  if (size < static_cast<int64_t>(kHeaderSize + kFooterSize)) return BookStatus::NotFound;

  uint8_t header[kHeaderSize];
  if (!readFully(storage, name, 0, header, sizeof(header))) {
    return BookStatus::IoError;
  }
  if (memcmp(header, "FIBP", 4) != 0) return BookStatus::Stale;
  if (getU16(header + 4) != kFormatVersion) return BookStatus::Stale;
  if (getU32(header + 8) != expectedHash) return BookStatus::Stale;

  // The trailing magic tells final ("FIBX", 24-byte footer) from partial
  // ("FIBx", 32-byte footer with build-progress fields) — see
  // PageCacheWriter::suspend(). A torn write matches neither: Stale.
  uint8_t magic[4];
  if (!readFully(storage, name, static_cast<uint32_t>(size) - 4, magic, sizeof(magic))) {
    return BookStatus::IoError;
  }
  isPartial_ = memcmp(magic, "FIBx", 4) == 0;
  buildBytesConsumed_ = 0;
  buildBytesTotal_ = 0;
  if (!isPartial_ && memcmp(magic, "FIBX", 4) != 0) return BookStatus::Stale;  // torn write
  const uint32_t footerSize = isPartial_ ? kPartialFooterSize : kFooterSize;
  if (size < static_cast<int64_t>(kHeaderSize + footerSize)) return BookStatus::Stale;

  uint8_t footer[kPartialFooterSize];
  if (!readFully(storage, name, static_cast<uint32_t>(size) - footerSize, footer, footerSize)) {
    return BookStatus::IoError;
  }
  indexOffset_ = getU32(footer);
  const uint32_t pageCount = getU32(footer + 4);
  const uint32_t anchorOffset = getU32(footer + 8);
  const uint32_t anchorCount = getU32(footer + 12);
  totalChars_ = getU32(footer + 16);
  if (isPartial_) {
    buildBytesConsumed_ = getU32(footer + 20);
    buildBytesTotal_ = getU32(footer + 24);
  }
  if (indexOffset_ < kHeaderSize ||
      static_cast<int64_t>(anchorOffset) + static_cast<int64_t>(anchorCount) * 8 + footerSize !=
          size ||
      static_cast<int64_t>(indexOffset_) + static_cast<int64_t>(pageCount) * 8 !=
          static_cast<int64_t>(anchorOffset)) {
    return BookStatus::Stale;
  }

  offsets_ = arena.allocArray<uint32_t>(pageCount);
  charStarts_ = arena.allocArray<uint32_t>(pageCount);
  if ((offsets_ == nullptr || charStarts_ == nullptr) && pageCount != 0) {
    return BookStatus::OutOfMemory;
  }
  for (uint32_t i = 0; i < pageCount; ++i) {
    uint8_t rec[8];
    if (!readFully(storage, name, indexOffset_ + i * 8, rec, sizeof(rec))) {
      return BookStatus::IoError;
    }
    offsets_[i] = getU32(rec);
    charStarts_[i] = getU32(rec + 4);
  }
  anchorHashes_ = arena.allocArray<uint32_t>(anchorCount);
  anchorChars_ = arena.allocArray<uint32_t>(anchorCount);
  if ((anchorHashes_ == nullptr || anchorChars_ == nullptr) && anchorCount != 0) {
    return BookStatus::OutOfMemory;
  }
  for (uint32_t a = 0; a < anchorCount; ++a) {
    uint8_t rec[8];
    if (!readFully(storage, name, anchorOffset + a * 8, rec, sizeof(rec))) {
      return BookStatus::IoError;
    }
    anchorHashes_[a] = getU32(rec);
    anchorChars_[a] = getU32(rec + 4);
  }
  anchorCount_ = anchorCount;
  pageCount_ = pageCount;
  return BookStatus::Ok;
}

bool PageCacheReader::charForAnchor(uint32_t idHash, uint32_t* charOut) const {
  for (uint32_t a = 0; a < anchorCount_; ++a) {
    if (anchorHashes_[a] == idHash) {
      *charOut = anchorChars_[a];
      return true;
    }
  }
  return false;
}

uint32_t PageCacheReader::pageForChar(uint32_t charOffset) const {
  if (pageCount_ == 0) return 0;
  // Last page whose charStart <= charOffset.
  uint32_t lo = 0;
  uint32_t hi = pageCount_ - 1;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo + 1) / 2;
    if (charStarts_[mid] <= charOffset) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

BookStatus PageCacheReader::readPage(uint32_t pageIndex, Arena& scratch, Page* out) {
  if (pageIndex >= pageCount_) return BookStatus::NotFound;
  const uint32_t blobOffset = offsets_[pageIndex];
  const uint32_t blobEnd = pageIndex + 1 < pageCount_ ? offsets_[pageIndex + 1] : indexOffset_;
  if (blobEnd <= blobOffset || blobEnd - blobOffset > kMaxBlobSize) return BookStatus::Stale;
  const uint32_t blobLen = blobEnd - blobOffset;

  uint8_t* blob = static_cast<uint8_t*>(scratch.alloc(blobLen, 4));
  if (blob == nullptr) return BookStatus::OutOfMemory;
  if (!readFully(*storage_, name_, blobOffset, blob, blobLen)) {
    return BookStatus::IoError;
  }
  return decodePageBlob(blob, blobLen, pageIndex, scratch, out);
}

// Decodes one serialized page blob into a Page whose records/strings live in
// `scratch`. Shared by PageCacheReader::readPage (final files) and
// PageCacheWriter::readPage (mid-build read-back of the open write stream).
static BookStatus decodePageBlob(const uint8_t* blob, uint32_t blobLen, uint32_t pageIndex,
                                 Arena& scratch, Page* out) {
  if (blobLen < 10) return BookStatus::Stale;
  const uint32_t charStart = getU32(blob);
  const uint16_t runCount = getU16(blob + 4);
  const uint16_t imageCount = getU16(blob + 6);
  const uint16_t linkCount = getU16(blob + 8);
  PageTextRun* runs = scratch.allocArray<PageTextRun>(runCount);
  if (runs == nullptr && runCount != 0) return BookStatus::OutOfMemory;
  PageImage* images = scratch.allocArray<PageImage>(imageCount);
  if (images == nullptr && imageCount != 0) return BookStatus::OutOfMemory;

  uint32_t pos = 10;
  for (uint16_t r = 0; r < runCount; ++r) {
    if (pos + 10 > blobLen) return BookStatus::Stale;
    PageTextRun& run = runs[r];
    run.x = static_cast<int16_t>(getU16(blob + pos));
    run.baselineY = static_cast<int16_t>(getU16(blob + pos + 2));
    run.sizePx = getU16(blob + pos + 4);
    run.styleFlags = blob[pos + 6];
    run.len = getU16(blob + pos + 8);
    pos += 10;
    if (pos + run.len > blobLen) return BookStatus::Stale;
    run.text = reinterpret_cast<const char*>(blob + pos);  // in-place, no copy
    pos += run.len;
  }
  for (uint16_t m = 0; m < imageCount; ++m) {
    if (pos + 10 > blobLen) return BookStatus::Stale;
    PageImage& img = images[m];
    img.x = static_cast<int16_t>(getU16(blob + pos));
    img.y = static_cast<int16_t>(getU16(blob + pos + 2));
    img.width = getU16(blob + pos + 4);
    img.height = getU16(blob + pos + 6);
    const uint16_t hrefLen = getU16(blob + pos + 8);
    pos += 10;
    if (pos + hrefLen + 1 > blobLen + 1 || pos + hrefLen > blobLen) return BookStatus::Stale;
    // NUL-terminate in place: hrefs are stored back to back, so borrow one
    // byte of the following record via a scratch copy instead.
    char* href = static_cast<char*>(scratch.alloc(hrefLen + 1, 1));
    if (href == nullptr) return BookStatus::OutOfMemory;
    memcpy(href, blob + pos, hrefLen);
    href[hrefLen] = '\0';
    img.href = href;
    pos += hrefLen;
  }

  PageLink* links = scratch.allocArray<PageLink>(linkCount);
  if (links == nullptr && linkCount != 0) return BookStatus::OutOfMemory;
  for (uint16_t l = 0; l < linkCount; ++l) {
    if (pos + 12 > blobLen) return BookStatus::Stale;
    PageLink& link = links[l];
    link.x = static_cast<int16_t>(getU16(blob + pos));
    link.y = static_cast<int16_t>(getU16(blob + pos + 2));
    link.width = getU16(blob + pos + 4);
    link.height = getU16(blob + pos + 6);
    const uint16_t tLen = getU16(blob + pos + 8);
    const uint16_t fLen = getU16(blob + pos + 10);
    pos += 12;
    if (pos + tLen + fLen > blobLen) return BookStatus::Stale;
    char* t = static_cast<char*>(scratch.alloc(tLen + 1u, 1));
    char* f = static_cast<char*>(scratch.alloc(fLen + 1u, 1));
    if (t == nullptr || f == nullptr) return BookStatus::OutOfMemory;
    memcpy(t, blob + pos, tLen);
    t[tLen] = 0;
    memcpy(f, blob + pos + tLen, fLen);
    f[fLen] = 0;
    link.target = t;
    link.fragment = f;
    pos += tLen + fLen;
  }

  out->runs = runs;
  out->runCount = runCount;
  out->images = images;
  out->imageCount = imageCount;
  out->links = links;
  out->linkCount = linkCount;
  out->pageIndex = pageIndex;
  out->charStart = charStart;
  return BookStatus::Ok;
}

}  // namespace book
}  // namespace freeink
