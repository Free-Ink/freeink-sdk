// FreeInkBook -- SD-backed container catalog: streaming index build, open,
// and the resident-table lookups (see BookCatalog.h for the file layout).

#include "BookCatalog.h"

#include <stdlib.h>
#include <string.h>

#include <new>

#include "epub/PackageParsers.h"
#include "epub/XmlSax.h"
#include "epub/XmlUtil.h"

namespace freeink {
namespace book {

using xmlutil::attrExact;
using xmlutil::attrLocal;
using xmlutil::hasToken;
using xmlutil::localIs;
using xmlutil::TextCapture;

namespace {

// v2: added the stored stylesheet section (compacted CssRule array), so open()
// never re-parses CSS from the container. Existing v1 catalog.fibc files read
// as Stale and rebuild.
constexpr uint32_t kCatalogVersion = 2;
constexpr uint32_t kMagic = 0x43424946u;  // 'F''I''B''C' little-endian
constexpr uint32_t kNoOffset = 0xFFFFFFFFu;
constexpr uint16_t kNoSpine16 = 0xFFFFu;
constexpr uint16_t kNoEntry16 = 0xFFFFu;
constexpr uint32_t kNoEntry32 = 0xFFFFFFFFu;
constexpr size_t kMaxCss = 16;
constexpr size_t kMaxPath = 512;
constexpr size_t kMaxZipNameLen = 1024;
constexpr size_t kMaxFieldLen = 256;

struct EntryRec {  // on-SD full entry record, indexed by hash-table row
  uint32_t localHeaderOffset;
  uint32_t compressedSize;
  uint32_t uncompressedSize;
  uint16_t method;
  uint16_t reserved;
};
static_assert(sizeof(EntryRec) == 16, "catalog entry record layout");

struct TocRec {
  uint32_t titleOffset;
  uint32_t fragmentOffset;  // kNoOffset = no anchor
  uint16_t spineIndex;      // kNoSpine16 = target is not a spine item
  uint8_t depth;
  uint8_t reserved;
};
static_assert(sizeof(TocRec) == 12, "catalog toc record layout");

struct Footer {
  uint32_t version;
  uint32_t fingerprint;
  uint32_t entryCount;
  uint32_t spineCount;
  uint32_t tocCount;
  uint32_t cssCount;
  uint32_t namePoolOff;
  uint32_t namePoolSize;
  uint32_t titlePoolOff;
  uint32_t titlePoolSize;
  uint32_t entryRecordsOff;
  uint32_t spineRecordsOff;
  uint32_t tocRecordsOff;
  uint32_t hashTableOff;
  uint32_t metaOff;
  uint32_t metaSize;
  uint32_t coverEntryIndex;  // kNoEntry32 = none
  uint32_t cssRulesOff;      // file offset of the compacted stylesheet section
  uint32_t cssRuleCount;     // number of CssRule rows stored
  uint32_t cssContentHash;   // stylesheet content hash (feeds layout generation)
  uint32_t magic;
};
static_assert(sizeof(Footer) == 84, "catalog footer layout");

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t fnv1a(uint32_t hash, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= p[i];
    hash *= 16777619u;
  }
  return hash;
}

bool readSourceFully(BookSource& source, uint64_t off, void* dst, uint32_t len) {
  uint8_t* p = static_cast<uint8_t*>(dst);
  uint32_t got = 0;
  while (got < len) {
    const int32_t n = source.readAt(off + got, p + got, len - got);
    if (n <= 0) return false;
    got += static_cast<uint32_t>(n);
  }
  return true;
}

bool readCacheFully(CacheStorage& cache, uint32_t off, void* dst, uint32_t len) {
  uint8_t* p = static_cast<uint8_t*>(dst);
  uint32_t got = 0;
  while (got < len) {
    const int32_t n = cache.readAt(BookCatalog::kCatalogName, off + got, p + got, len - got);
    if (n <= 0) return false;
    got += static_cast<uint32_t>(n);
  }
  return true;
}

// Fingerprint of the container the catalog was built from: FNV-1a over the
// raw central-directory bytes plus the directory geometry and the catalog
// format version. Any repack, edit, or replacement changes it.
BookStatus fingerprintContainer(BookSource& source, Arena& scratch, uint32_t* out) {
  uint32_t dirOffset = 0;
  uint32_t dirSize = 0;
  uint16_t entryCount = 0;
  const BookStatus located =
      ZipCatalog::locateCentralDirectory(source, &dirOffset, &dirSize, &entryCount);
  if (located != BookStatus::Ok) return located;

  const size_t marked = scratch.mark();
  uint8_t* buf = static_cast<uint8_t*>(scratch.alloc(2048, 4));
  if (buf == nullptr) return BookStatus::OutOfMemory;
  uint32_t hash = 2166136261u;
  uint32_t done = 0;
  while (done < dirSize) {
    const uint32_t want = dirSize - done < 2048 ? dirSize - done : 2048;
    if (!readSourceFully(source, static_cast<uint64_t>(dirOffset) + done, buf, want)) {
      scratch.release(marked);
      return BookStatus::IoError;
    }
    hash = fnv1a(hash, buf, want);
    done += want;
  }
  scratch.release(marked);
  const uint64_t fileSize = source.size();
  hash = fnv1a(hash, &dirOffset, sizeof(dirOffset));
  hash = fnv1a(hash, &entryCount, sizeof(entryCount));
  hash = fnv1a(hash, &fileSize, sizeof(fileSize));
  const uint32_t version = kCatalogVersion;
  hash = fnv1a(hash, &version, sizeof(version));
  *out = hash;
  return BookStatus::Ok;
}

// --- build: central-directory scan -------------------------------------------

// Everything the build needs per ZIP entry, hashed name instead of the name
// string -- 20 bytes rather than the ~60 the in-RAM catalog spends.
struct BuildEntry {
  uint32_t nameHash;
  uint32_t localHeaderOffset;
  uint32_t compressedSize;
  uint32_t uncompressedSize;
  uint16_t method;
  uint16_t pad;
};

int cmpBuildEntry(const void* a, const void* b) {
  const uint32_t ha = static_cast<const BuildEntry*>(a)->nameHash;
  const uint32_t hb = static_cast<const BuildEntry*>(b)->nameHash;
  return ha < hb ? -1 : (ha > hb ? 1 : 0);
}

// Buffered forward reader over the central directory (one readAt per KB, not
// per field). The buffer comes from the caller's scratch.
class CdReader {
 public:
  CdReader(BookSource& source, uint64_t offset, uint8_t* buf, uint32_t cap)
      : source_(source), pos_(offset), buf_(buf), cap_(cap) {}

  bool read(void* dst, uint32_t len) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    while (len > 0) {
      if (avail_ == 0 && !refill()) return false;
      const uint32_t take = avail_ < len ? avail_ : len;
      memcpy(out, buf_ + bufPos_, take);
      out += take;
      bufPos_ += take;
      avail_ -= take;
      len -= take;
    }
    return true;
  }

  // Consumes len bytes, folding them into an FNV-1a hash (entry names are
  // hashed in place -- never staged whole).
  bool readHashed(uint32_t len, uint32_t* hash) {
    uint32_t h = *hash;
    while (len > 0) {
      if (avail_ == 0 && !refill()) return false;
      const uint32_t take = avail_ < len ? avail_ : len;
      h = fnv1a(h, buf_ + bufPos_, take);
      bufPos_ += take;
      avail_ -= take;
      len -= take;
    }
    *hash = h;
    return true;
  }

  void skip(uint32_t len) {
    const uint32_t buffered = avail_ < len ? avail_ : len;
    bufPos_ += buffered;
    avail_ -= buffered;
    pos_ += len - buffered;
  }

 private:
  bool refill() {
    const int32_t n = source_.readAt(pos_, buf_, cap_);
    if (n <= 0) return false;
    avail_ = static_cast<uint32_t>(n);
    bufPos_ = 0;
    pos_ += static_cast<uint32_t>(n);
    return true;
  }

  BookSource& source_;
  uint64_t pos_;
  uint8_t* buf_;
  uint32_t cap_;
  uint32_t bufPos_ = 0;
  uint32_t avail_ = 0;
};

// Streams the central directory into a hash-sorted BuildEntry table in
// `scratch`. Table position after sorting IS the catalog's entry index.
BookStatus scanCentralDirectory(BookSource& source, Arena& scratch, BuildEntry** entriesOut,
                                uint16_t* countOut) {
  uint32_t dirOffset = 0;
  uint32_t dirSize = 0;
  uint16_t count = 0;
  const BookStatus located =
      ZipCatalog::locateCentralDirectory(source, &dirOffset, &dirSize, &count);
  if (located != BookStatus::Ok) return located;

  BuildEntry* entries = scratch.allocArray<BuildEntry>(count);
  if (entries == nullptr && count != 0) return BookStatus::OutOfMemory;

  const size_t marked = scratch.mark();
  uint8_t* buf = static_cast<uint8_t*>(scratch.alloc(1024, 4));
  if (buf == nullptr) return BookStatus::OutOfMemory;
  CdReader reader(source, dirOffset, buf, 1024);
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t hdr[46];
    if (!reader.read(hdr, sizeof(hdr))) {
      scratch.release(marked);
      return BookStatus::Truncated;
    }
    if (le32(hdr) != 0x02014b50u) {
      scratch.release(marked);
      return BookStatus::NotZip;
    }
    const uint16_t nameLen = le16(hdr + 28);
    const uint16_t extraLen = le16(hdr + 30);
    const uint16_t commentLen = le16(hdr + 32);
    if (nameLen == 0 || nameLen > kMaxZipNameLen) {
      scratch.release(marked);
      return BookStatus::Unsupported;
    }
    BuildEntry& e = entries[i];
    e.method = le16(hdr + 10);
    e.compressedSize = le32(hdr + 20);
    e.uncompressedSize = le32(hdr + 24);
    e.localHeaderOffset = le32(hdr + 42);
    e.pad = 0;
    e.nameHash = 2166136261u;  // FNV-1a basis, matches ZipCatalog::hashPath
    if (!reader.readHashed(nameLen, &e.nameHash)) {
      scratch.release(marked);
      return BookStatus::Truncated;
    }
    reader.skip(static_cast<uint32_t>(extraLen) + commentLen);
  }
  scratch.release(marked);

  if (count > 0) qsort(entries, count, sizeof(BuildEntry), cmpBuildEntry);
  *entriesOut = entries;
  *countOut = count;
  return BookStatus::Ok;
}

// First hash-sorted table row matching `hash`, or -1.
int findBuildEntry(const BuildEntry* entries, uint16_t count, uint32_t hash) {
  int lo = 0;
  int hi = static_cast<int>(count) - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    if (entries[mid].nameHash < hash) {
      lo = mid + 1;
    } else if (entries[mid].nameHash > hash) {
      hi = mid - 1;
    } else {
      int row = mid;
      while (row > 0 && entries[row - 1].nameHash == hash) --row;
      return row;
    }
  }
  return -1;
}

ZipEntry toZipEntry(const BuildEntry& e) {
  ZipEntry z;
  z.name = "";
  z.nameHash = e.nameHash;
  z.compressedSize = e.compressedSize;
  z.uncompressedSize = e.uncompressedSize;
  z.localHeaderOffset = e.localHeaderOffset;
  z.method = e.method;
  return z;
}

// First spine slot referencing `entryIdx`, or kNoSpine16.
uint16_t spineForEntry(uint16_t entryIdx, const uint16_t* spineEntryIdx, size_t spineUsed) {
  if (entryIdx == kNoEntry16) return kNoSpine16;
  for (size_t s = 0; s < spineUsed; ++s) {
    if (spineEntryIdx[s] == entryIdx) return static_cast<uint16_t>(s);
  }
  return kNoSpine16;
}

// --- build: streaming section writer -----------------------------------------

class SectionWriter {
 public:
  explicit SectionWriter(CacheStorage& cache) : cache_(cache) {}

  bool begin() {
    ok_ = cache_.beginWrite(BookCatalog::kCatalogName);
    return ok_;
  }
  bool write(const void* data, uint32_t len) {
    if (ok_ && len > 0) {
      ok_ = cache_.write(data, len);
      if (ok_) offset_ += len;
    }
    return ok_;
  }
  bool writeStr(const char* s) { return write(s, static_cast<uint32_t>(strlen(s)) + 1); }
  bool end() {
    const bool committed = ok_ && cache_.endWrite();
    ok_ = false;
    return committed;
  }
  void abort() {
    cache_.endWrite();
    cache_.remove(BookCatalog::kCatalogName);
    ok_ = false;
  }
  uint32_t offset() const { return offset_; }
  bool ok() const { return ok_; }

 private:
  CacheStorage& cache_;
  uint32_t offset_ = 0;
  bool ok_ = false;
};

// --- build: OPF handlers ------------------------------------------------------

// Captured book metadata -- fixed buffers, not arena strings, so the count
// handler can be released wholesale.
struct BuildMeta {
  char title[kMaxFieldLen] = "";
  char author[kMaxFieldLen] = "";
  char language[kMaxFieldLen] = "";
  char identifier[kMaxFieldLen] = "";
};

class OpfCountHandler : public XmlHandler {
 public:
  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "metadata")) {
      ++inMetadata_;
    } else if (localIs(name, "manifest")) {
      ++inManifest_;
    } else if (localIs(name, "spine")) {
      ++inSpine_;
      if (tocIdHash == 0) {
        const char* toc = attrLocal(atts, "toc");
        if (toc != nullptr) tocIdHash = ZipCatalog::hashPath(toc);
      }
    } else if (inMetadata_ > 0 && localIs(name, "meta")) {
      // EPUB 2 cover declaration: <meta name="cover" content="manifest-id"/>.
      const char* metaName = attrLocal(atts, "name");
      const char* content = attrLocal(atts, "content");
      if (metaName != nullptr && content != nullptr && strcmp(metaName, "cover") == 0) {
        coverIdHash = ZipCatalog::hashPath(content);
      }
    } else if (inMetadata_ > 0) {
      captureSlot_ = nullptr;
      if (localIs(name, "title")) captureSlot_ = meta.title;
      else if (localIs(name, "creator")) captureSlot_ = meta.author;
      else if (localIs(name, "language")) captureSlot_ = meta.language;
      else if (localIs(name, "identifier")) captureSlot_ = meta.identifier;
      if (captureSlot_ != nullptr) capture_.begin();
    } else if (inManifest_ > 0 && localIs(name, "item")) {
      ++manifestCount;
    } else if (inSpine_ > 0 && localIs(name, "itemref")) {
      ++spineCount;
    }
  }

  void onEndElement(const char* name) override {
    if (localIs(name, "metadata")) {
      if (inMetadata_ > 0) --inMetadata_;
    } else if (localIs(name, "manifest")) {
      if (inManifest_ > 0) --inManifest_;
    } else if (localIs(name, "spine")) {
      if (inSpine_ > 0) --inSpine_;
    } else if (capture_.active) {
      const char* text = capture_.trimmed();
      // Keep the first non-empty occurrence (books often repeat dc: fields).
      if (text[0] != '\0' && captureSlot_ != nullptr && captureSlot_[0] == '\0') {
        strncpy(captureSlot_, text, kMaxFieldLen - 1);
        captureSlot_[kMaxFieldLen - 1] = '\0';
      }
      capture_.end();
      captureSlot_ = nullptr;
    }
  }

  void onText(const char* text, int len) override { capture_.add(text, len); }

  size_t manifestCount = 0;
  size_t spineCount = 0;
  uint32_t tocIdHash = 0;
  uint32_t coverIdHash = 0;
  BuildMeta meta;

 private:
  int inMetadata_ = 0;
  int inManifest_ = 0;
  int inSpine_ = 0;
  char* captureSlot_ = nullptr;
  TextCapture capture_;
};

// Fill pass: streams every manifest href into the name pool on SD, resolves
// items to central-directory entries by name hash, and resolves the spine.
// Manifest strings never persist in RAM.
class OpfFillHandler : public XmlHandler {
 public:
  OpfFillHandler(Arena& strArena, SectionWriter& pool, const BuildEntry* entries,
                 uint16_t entryCount, const char* opfDir)
      : strArena_(strArena), pool_(pool), entries_(entries), entryCount_(entryCount),
        opfDir_(opfDir) {}

  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "manifest")) {
      ++inManifest_;
    } else if (localIs(name, "spine")) {
      ++inSpine_;
    } else if (inManifest_ > 0 && localIs(name, "item")) {
      onItem(atts);
    } else if (inSpine_ > 0 && localIs(name, "itemref")) {
      onItemref(atts);
    }
  }

  void onEndElement(const char* name) override {
    if (localIs(name, "manifest")) {
      if (inManifest_ > 0) --inManifest_;
    } else if (localIs(name, "spine")) {
      if (inSpine_ > 0) --inSpine_;
    }
  }

  // Wired by the caller before parsing.
  uint32_t* mIdHash = nullptr;
  uint32_t* mNameOff = nullptr;
  uint16_t* mEntryIdx = nullptr;
  size_t manifestCap = 0;
  size_t manifestUsed = 0;
  uint16_t* spineEntryIdx = nullptr;
  uint32_t* spineNameOff = nullptr;
  size_t spineCap = 0;
  size_t spineUsed = 0;
  uint32_t tocIdHash = 0;  // from the count pass

  uint16_t cssEntryIdx[kMaxCss];
  size_t cssUsed = 0;
  uint32_t coverEntryIdx = kNoEntry32;  // properties="cover-image"
  int navManifestIdx = -1;
  int ncxManifestIdx = -1;
  char* tocDocHref = nullptr;  // caller buffer, kMaxPath -- href of nav/NCX doc
  bool navHrefStored = false;
  bool failed = false;

 private:
  void onItem(const char** atts) {
    if (manifestUsed >= manifestCap) return;
    const char* id = attrLocal(atts, "id");
    const char* href = attrLocal(atts, "href");
    if (id == nullptr || href == nullptr) return;

    const size_t marked = strArena_.mark();
    const char* resolved = resolveHref(strArena_, opfDir_, href, nullptr);
    if (resolved == nullptr) {
      strArena_.release(marked);
      failed = true;
      return;
    }
    const uint32_t hrefHash = ZipCatalog::hashPath(resolved);
    const int row = findBuildEntry(entries_, entryCount_, hrefHash);

    const uint32_t nameOffset = pool_.offset();
    if (!pool_.writeStr(resolved)) failed = true;

    const size_t mi = manifestUsed++;
    mIdHash[mi] = ZipCatalog::hashPath(id);
    mNameOff[mi] = nameOffset;
    mEntryIdx[mi] = row >= 0 ? static_cast<uint16_t>(row) : kNoEntry16;

    const char* properties = attrLocal(atts, "properties");
    const char* mediaType = attrLocal(atts, "media-type");
    if (hasToken(properties, "nav") && navManifestIdx < 0) {
      navManifestIdx = static_cast<int>(mi);
      copyTocHref(resolved, /*isNav=*/true);
    }
    const bool isNcxType =
        mediaType != nullptr && strcmp(mediaType, "application/x-dtbncx+xml") == 0;
    const bool idMatchesToc = tocIdHash != 0 && mIdHash[mi] == tocIdHash;
    if (ncxManifestIdx < 0 && (idMatchesToc || isNcxType)) {
      ncxManifestIdx = static_cast<int>(mi);
      copyTocHref(resolved, /*isNav=*/false);
    }
    if (hasToken(properties, "cover-image") && coverEntryIdx == kNoEntry32 && row >= 0) {
      coverEntryIdx = static_cast<uint32_t>(row);
    }
    if (mediaType != nullptr && strcmp(mediaType, "text/css") == 0 && row >= 0 &&
        cssUsed < kMaxCss) {
      cssEntryIdx[cssUsed++] = static_cast<uint16_t>(row);
    }
    strArena_.release(marked);
  }

  void onItemref(const char** atts) {
    if (spineUsed >= spineCap) return;
    const char* idref = attrLocal(atts, "idref");
    if (idref == nullptr) return;
    const uint32_t idHash = ZipCatalog::hashPath(idref);
    for (size_t mi = 0; mi < manifestUsed; ++mi) {
      if (mIdHash[mi] == idHash) {
        spineEntryIdx[spineUsed] = mEntryIdx[mi];
        spineNameOff[spineUsed] = mNameOff[mi];
        ++spineUsed;
        return;
      }
    }
    // Unresolvable idrefs are dropped, matching the in-RAM parser.
  }

  // The nav document wins over the NCX; remember whichever is current best.
  void copyTocHref(const char* resolved, bool isNav) {
    if (tocDocHref == nullptr) return;
    if (!isNav && navHrefStored) return;
    strncpy(tocDocHref, resolved, kMaxPath - 1);
    tocDocHref[kMaxPath - 1] = '\0';
    if (isNav) navHrefStored = true;
  }

  Arena& strArena_;
  SectionWriter& pool_;
  const BuildEntry* entries_;
  uint16_t entryCount_;
  const char* opfDir_;
  int inManifest_ = 0;
  int inSpine_ = 0;
};

// --- build: TOC handlers -------------------------------------------------------

class NavTocCounter : public XmlHandler {
 public:
  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "nav")) {
      ++navDepth_;
      if (!inToc_ && navDepth_ == 1 && hasToken(attrExact(atts, "epub:type"), "toc")) {
        inToc_ = true;
      }
      return;
    }
    if (!inToc_) return;
    if (localIs(name, "a") && attrLocal(atts, "href") != nullptr) ++count;
  }
  void onEndElement(const char* name) override {
    if (localIs(name, "nav")) {
      if (navDepth_ > 0) --navDepth_;
      if (navDepth_ == 0) inToc_ = false;
    }
  }
  size_t count = 0;

 private:
  int navDepth_ = 0;
  bool inToc_ = false;
};

class NcxTocCounter : public XmlHandler {
 public:
  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "navPoint")) {
      ++pointDepth_;
    } else if (pointDepth_ > 0 && localIs(name, "content") &&
               attrLocal(atts, "src") != nullptr) {
      ++count;
    }
  }
  void onEndElement(const char* name) override {
    if (localIs(name, "navPoint") && pointDepth_ > 0) --pointDepth_;
  }
  size_t count = 0;

 private:
  int pointDepth_ = 0;
};

// Shared fill-pass state: streams titles/fragments into the title pool and
// emits fixed-size records with the target resolved to a spine index -- the
// href string itself is never retained.
struct TocFillContext {
  Arena* strArena = nullptr;
  SectionWriter* pool = nullptr;
  uint32_t poolBase = 0;  // file offset where the title pool starts
  const BuildEntry* entries = nullptr;
  uint16_t entryCount = 0;
  const uint16_t* spineEntryIdx = nullptr;
  size_t spineUsed = 0;
  const char* docDir = nullptr;
  TocRec* recs = nullptr;
  size_t cap = 0;
  size_t used = 0;
  bool failed = false;

  void emit(const char* title, const char* href, const char* fragment, uint8_t depth) {
    if (used >= cap) return;
    TocRec& rec = recs[used];
    rec.titleOffset = pool->offset() - poolBase;
    if (!pool->writeStr(title)) failed = true;
    if (fragment != nullptr && fragment[0] != '\0') {
      rec.fragmentOffset = pool->offset() - poolBase;
      if (!pool->writeStr(fragment)) failed = true;
    } else {
      rec.fragmentOffset = kNoOffset;
    }
    const int row = findBuildEntry(entries, entryCount, ZipCatalog::hashPath(href));
    rec.spineIndex = row >= 0
                         ? spineForEntry(static_cast<uint16_t>(row), spineEntryIdx, spineUsed)
                         : kNoSpine16;
    rec.depth = depth;
    rec.reserved = 0;
    ++used;
  }
};

class NavTocFiller : public XmlHandler {
 public:
  explicit NavTocFiller(TocFillContext& ctx) : ctx_(ctx) {}

  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "nav")) {
      ++navDepth_;
      if (!inToc_ && navDepth_ == 1 && hasToken(attrExact(atts, "epub:type"), "toc")) {
        inToc_ = true;
      }
      return;
    }
    if (!inToc_) return;
    if (localIs(name, "ol")) {
      if (olDepth_ < 255) ++olDepth_;
    } else if (localIs(name, "a") && !inAnchor_) {
      const char* href = attrLocal(atts, "href");
      if (href == nullptr) return;
      const size_t marked = ctx_.strArena->mark();
      const char* fragment = nullptr;
      const char* resolved = resolveHref(*ctx_.strArena, ctx_.docDir, href, &fragment);
      if (resolved == nullptr) {
        ctx_.strArena->release(marked);
        ctx_.failed = true;
        return;
      }
      strncpy(curHref_, resolved, sizeof(curHref_) - 1);
      curHref_[sizeof(curHref_) - 1] = '\0';
      if (fragment != nullptr) {
        strncpy(curFrag_, fragment, sizeof(curFrag_) - 1);
        curFrag_[sizeof(curFrag_) - 1] = '\0';
      } else {
        curFrag_[0] = '\0';
      }
      ctx_.strArena->release(marked);
      capture_.begin();
      inAnchor_ = true;
    }
  }

  void onEndElement(const char* name) override {
    if (localIs(name, "nav")) {
      if (navDepth_ > 0) --navDepth_;
      if (navDepth_ == 0) inToc_ = false;
      return;
    }
    if (!inToc_) return;
    if (localIs(name, "ol")) {
      if (olDepth_ > 0) --olDepth_;
    } else if (localIs(name, "a") && inAnchor_) {
      inAnchor_ = false;
      ctx_.emit(capture_.trimmed(), curHref_, curFrag_,
                static_cast<uint8_t>(olDepth_ > 0 ? olDepth_ - 1 : 0));
      capture_.end();
    }
  }

  void onText(const char* text, int len) override {
    if (inAnchor_) capture_.add(text, len);
  }

 private:
  TocFillContext& ctx_;
  int navDepth_ = 0;
  int olDepth_ = 0;
  bool inToc_ = false;
  bool inAnchor_ = false;
  char curHref_[kMaxPath];
  char curFrag_[kMaxFieldLen];
  TextCapture capture_;
};

class NcxTocFiller : public XmlHandler {
 public:
  explicit NcxTocFiller(TocFillContext& ctx) : ctx_(ctx) {}

  void onStartElement(const char* name, const char** atts) override {
    if (localIs(name, "navPoint")) {
      if (pointDepth_ < 255) ++pointDepth_;
      pendingTitle_[0] = '\0';
    } else if (localIs(name, "navLabel")) {
      ++inLabel_;
    } else if (inLabel_ > 0 && localIs(name, "text")) {
      capture_.begin();
    } else if (pointDepth_ > 0 && localIs(name, "content")) {
      const char* src = attrLocal(atts, "src");
      if (src == nullptr) return;
      const size_t marked = ctx_.strArena->mark();
      const char* fragment = nullptr;
      const char* resolved = resolveHref(*ctx_.strArena, ctx_.docDir, src, &fragment);
      if (resolved == nullptr) {
        ctx_.strArena->release(marked);
        ctx_.failed = true;
        return;
      }
      ctx_.emit(pendingTitle_, resolved, fragment, static_cast<uint8_t>(pointDepth_ - 1));
      ctx_.strArena->release(marked);
    }
  }

  void onEndElement(const char* name) override {
    if (localIs(name, "navPoint")) {
      if (pointDepth_ > 0) --pointDepth_;
    } else if (localIs(name, "navLabel")) {
      if (inLabel_ > 0) --inLabel_;
    } else if (capture_.active && localIs(name, "text")) {
      const char* text = capture_.trimmed();
      const size_t n = strlen(text);
      memcpy(pendingTitle_, text, n + 1);
      capture_.end();
    }
  }

  void onText(const char* text, int len) override { capture_.add(text, len); }

 private:
  TocFillContext& ctx_;
  int pointDepth_ = 0;
  int inLabel_ = 0;
  char pendingTitle_[xmlutil::kMaxTextCapture + 1] = "";
  TextCapture capture_;
};

}  // namespace

// --- build ---------------------------------------------------------------------

BookStatus BookCatalog::build(BookSource& source, CacheStorage& cache, Arena& scratch,
                              Arena* parseArena) {
  Arena& parse = parseArena != nullptr ? *parseArena : scratch;

  uint32_t fingerprint = 0;
  BookStatus st = fingerprintContainer(source, scratch, &fingerprint);
  if (st != BookStatus::Ok) return st;

  BuildEntry* entries = nullptr;
  uint16_t entryCount = 0;
  st = scanCentralDirectory(source, scratch, &entries, &entryCount);
  if (st != BookStatus::Ok) return st;

  // DRM scan -- same policy as Book::open.
  {
    const int encRow =
        findBuildEntry(entries, entryCount, ZipCatalog::hashPath("META-INF/encryption.xml"));
    if (encRow >= 0) {
      xmlutil::EncryptionScan scan;
      const ZipEntry encEntry = toZipEntry(entries[encRow]);
      const size_t marked = parse.mark();
      const BookStatus encStatus = XmlSax::parseEntry(source, encEntry, parse, scan);
      parse.release(marked);
      if (encStatus != BookStatus::Ok || scan.contentEncrypted) return BookStatus::Encrypted;
    }
  }

  // container.xml → OPF path.
  char* opfPath = static_cast<char*>(scratch.alloc(kMaxPath, 1));
  char* opfDir = static_cast<char*>(scratch.alloc(kMaxPath, 1));
  char* tocDocHref = static_cast<char*>(scratch.alloc(kMaxPath, 1));
  if (opfPath == nullptr || opfDir == nullptr || tocDocHref == nullptr) {
    return BookStatus::OutOfMemory;
  }
  tocDocHref[0] = '\0';
  {
    const int row =
        findBuildEntry(entries, entryCount, ZipCatalog::hashPath("META-INF/container.xml"));
    if (row < 0) return BookStatus::NotEpub;
    const ZipEntry containerEntry = toZipEntry(entries[row]);
    const size_t marked = scratch.mark();
    const char* parsedPath = nullptr;
    st = parseContainer(source, containerEntry, scratch, parse, &parsedPath);
    if (st != BookStatus::Ok) return st;
    strncpy(opfPath, parsedPath, kMaxPath - 1);
    opfPath[kMaxPath - 1] = '\0';
    scratch.release(marked);
  }
  const int opfRow = findBuildEntry(entries, entryCount, ZipCatalog::hashPath(opfPath));
  if (opfRow < 0) return BookStatus::NotEpub;
  const ZipEntry opfEntry = toZipEntry(entries[opfRow]);
  if (!dirName(opfPath, opfDir, kMaxPath)) return BookStatus::Unsupported;

  // OPF count pass: array sizes + metadata + toc/cover ids.
  size_t manifestCount = 0;
  size_t spineCount = 0;
  uint32_t tocIdHash = 0;
  uint32_t coverIdHashMeta = 0;
  BuildMeta* meta = static_cast<BuildMeta*>(scratch.alloc(sizeof(BuildMeta), 1));
  if (meta == nullptr) return BookStatus::OutOfMemory;
  {
    const size_t marked = scratch.mark();
    void* counterMem = scratch.alloc(sizeof(OpfCountHandler), alignof(OpfCountHandler));
    if (counterMem == nullptr) return BookStatus::OutOfMemory;
    OpfCountHandler* counter = new (counterMem) OpfCountHandler;
    st = XmlSax::parseEntry(source, opfEntry, parse, *counter);
    if (st != BookStatus::Ok) return st;
    manifestCount = counter->manifestCount;
    spineCount = counter->spineCount;
    tocIdHash = counter->tocIdHash;
    coverIdHashMeta = counter->coverIdHash;
    *meta = counter->meta;
    scratch.release(marked);
  }
  if (manifestCount == 0 || spineCount == 0) return BookStatus::NotEpub;
  if (manifestCount >= kNoEntry16 || spineCount >= kNoSpine16) return BookStatus::Unsupported;

  // Spine tables survive until the records are written; manifest tables are
  // released right after the fill pass.
  uint16_t* spineEntryIdx = scratch.allocArray<uint16_t>(spineCount);
  uint32_t* spineNameOff = scratch.allocArray<uint32_t>(spineCount);
  if (spineEntryIdx == nullptr || spineNameOff == nullptr) return BookStatus::OutOfMemory;
  size_t spineUsed = 0;

  uint16_t cssEntryIdx[kMaxCss];
  size_t cssUsed = 0;
  uint32_t coverEntryIdx = kNoEntry32;
  bool tocIsNav = false;
  bool haveTocDoc = false;

  SectionWriter writer(cache);
  if (!writer.begin()) return BookStatus::IoError;

  // Section 1 (file offset 0): name pool, written by the OPF fill pass.
  const uint32_t namePoolOff = 0;
  uint32_t namePoolSize = 0;
  {
    const size_t marked = scratch.mark();
    uint32_t* mIdHash = scratch.allocArray<uint32_t>(manifestCount);
    uint32_t* mNameOff = scratch.allocArray<uint32_t>(manifestCount);
    uint16_t* mEntryIdx = scratch.allocArray<uint16_t>(manifestCount);
    OpfFillHandler* filler = static_cast<OpfFillHandler*>(
        scratch.alloc(sizeof(OpfFillHandler), alignof(OpfFillHandler)));
    if (mIdHash == nullptr || mNameOff == nullptr || mEntryIdx == nullptr ||
        filler == nullptr) {
      writer.abort();
      return BookStatus::OutOfMemory;
    }
    new (filler) OpfFillHandler(parse, writer, entries, entryCount, opfDir);
    filler->mIdHash = mIdHash;
    filler->mNameOff = mNameOff;
    filler->mEntryIdx = mEntryIdx;
    filler->manifestCap = manifestCount;
    filler->spineEntryIdx = spineEntryIdx;
    filler->spineNameOff = spineNameOff;
    filler->spineCap = spineCount;
    filler->tocIdHash = tocIdHash;
    filler->tocDocHref = tocDocHref;

    st = XmlSax::parseEntry(source, opfEntry, parse, *filler);
    if (st != BookStatus::Ok || filler->failed || !writer.ok()) {
      writer.abort();
      return st != BookStatus::Ok ? st : BookStatus::IoError;
    }
    spineUsed = filler->spineUsed;
    cssUsed = filler->cssUsed;
    memcpy(cssEntryIdx, filler->cssEntryIdx, sizeof(cssEntryIdx));
    coverEntryIdx = filler->coverEntryIdx;
    tocIsNav = filler->navManifestIdx >= 0;
    haveTocDoc = filler->navManifestIdx >= 0 || filler->ncxManifestIdx >= 0;

    // EPUB 2 cover metadata names a manifest id rather than a property.
    if (coverEntryIdx == kNoEntry32 && coverIdHashMeta != 0) {
      for (size_t mi = 0; mi < filler->manifestUsed; ++mi) {
        if (mIdHash[mi] == coverIdHashMeta && mEntryIdx[mi] != kNoEntry16) {
          coverEntryIdx = mEntryIdx[mi];
          break;
        }
      }
    }
    scratch.release(marked);
    namePoolSize = writer.offset() - namePoolOff;
  }
  if (spineUsed == 0) {
    writer.abort();
    return BookStatus::NotEpub;
  }

  // Section 2: title pool + TOC records (records staged in scratch until the
  // pool section is complete -- one write stream, so sections are sequential).
  const uint32_t titlePoolOff = writer.offset();
  TocRec* tocRecs = nullptr;
  size_t tocUsed = 0;
  if (haveTocDoc && tocDocHref[0] != '\0') {
    const int tocRow = findBuildEntry(entries, entryCount, ZipCatalog::hashPath(tocDocHref));
    if (tocRow >= 0) {
      const ZipEntry tocEntry = toZipEntry(entries[tocRow]);
      char* tocDir = static_cast<char*>(scratch.alloc(kMaxPath, 1));
      if (tocDir == nullptr || !dirName(tocDocHref, tocDir, kMaxPath)) {
        writer.abort();
        return tocDir == nullptr ? BookStatus::OutOfMemory : BookStatus::Unsupported;
      }

      size_t tocCount = 0;
      {
        const size_t marked = scratch.mark();
        if (tocIsNav) {
          void* mem = scratch.alloc(sizeof(NavTocCounter), alignof(NavTocCounter));
          if (mem == nullptr) {
            writer.abort();
            return BookStatus::OutOfMemory;
          }
          NavTocCounter* counter = new (mem) NavTocCounter;
          st = XmlSax::parseEntry(source, tocEntry, parse, *counter);
          tocCount = counter->count;
        } else {
          void* mem = scratch.alloc(sizeof(NcxTocCounter), alignof(NcxTocCounter));
          if (mem == nullptr) {
            writer.abort();
            return BookStatus::OutOfMemory;
          }
          NcxTocCounter* counter = new (mem) NcxTocCounter;
          st = XmlSax::parseEntry(source, tocEntry, parse, *counter);
          tocCount = counter->count;
        }
        scratch.release(marked);
      }
      if (st != BookStatus::Ok) {
        writer.abort();
        return st;
      }

      if (tocCount > 0) {
        tocRecs = scratch.allocArray<TocRec>(tocCount);
        if (tocRecs == nullptr) {
          writer.abort();
          return BookStatus::OutOfMemory;
        }
        TocFillContext ctx;
        ctx.strArena = &parse;
        ctx.pool = &writer;
        ctx.poolBase = titlePoolOff;
        ctx.entries = entries;
        ctx.entryCount = entryCount;
        ctx.spineEntryIdx = spineEntryIdx;
        ctx.spineUsed = spineUsed;
        ctx.docDir = tocDir;
        ctx.recs = tocRecs;
        ctx.cap = tocCount;

        const size_t marked = scratch.mark();
        if (tocIsNav) {
          NavTocFiller* filler = static_cast<NavTocFiller*>(
              scratch.alloc(sizeof(NavTocFiller), alignof(NavTocFiller)));
          if (filler == nullptr) {
            writer.abort();
            return BookStatus::OutOfMemory;
          }
          new (filler) NavTocFiller(ctx);
          st = XmlSax::parseEntry(source, tocEntry, parse, *filler);
        } else {
          NcxTocFiller* filler = static_cast<NcxTocFiller*>(
              scratch.alloc(sizeof(NcxTocFiller), alignof(NcxTocFiller)));
          if (filler == nullptr) {
            writer.abort();
            return BookStatus::OutOfMemory;
          }
          new (filler) NcxTocFiller(ctx);
          st = XmlSax::parseEntry(source, tocEntry, parse, *filler);
        }
        scratch.release(marked);
        if (st != BookStatus::Ok || ctx.failed || !writer.ok()) {
          writer.abort();
          return st != BookStatus::Ok ? st : BookStatus::IoError;
        }
        tocUsed = ctx.used;
      }
    }
  }
  const uint32_t titlePoolSize = writer.offset() - titlePoolOff;

  // Section 3: full entry records (16 B each, hash-table row order).
  const uint32_t entryRecordsOff = writer.offset();
  for (uint16_t i = 0; i < entryCount; ++i) {
    EntryRec rec;
    rec.localHeaderOffset = entries[i].localHeaderOffset;
    rec.compressedSize = entries[i].compressedSize;
    rec.uncompressedSize = entries[i].uncompressedSize;
    rec.method = entries[i].method;
    rec.reserved = 0;
    if (!writer.write(&rec, sizeof(rec))) break;
  }

  // Section 4: resident spine records.
  const uint32_t spineRecordsOff = writer.offset();
  for (size_t s = 0; s < spineUsed; ++s) {
    SpineRec rec;
    rec.nameOffset = spineNameOff[s];
    rec.entryIndex = spineEntryIdx[s];
    if (rec.entryIndex != kNoEntry16) {
      const BuildEntry& e = entries[rec.entryIndex];
      rec.localHeaderOffset = e.localHeaderOffset;
      rec.uncompressedSize = e.uncompressedSize;
      rec.method = e.method;
    } else {
      rec.localHeaderOffset = 0;
      rec.uncompressedSize = 0;
      rec.method = 0;
    }
    if (!writer.write(&rec, sizeof(rec))) break;
  }

  // Section 5: TOC records.
  const uint32_t tocRecordsOff = writer.offset();
  if (tocUsed > 0) {
    writer.write(tocRecs, static_cast<uint32_t>(tocUsed * sizeof(TocRec)));
  }

  // Section 6: resident hash table (entry index == row).
  const uint32_t hashTableOff = writer.offset();
  {
    const size_t marked = scratch.mark();
    uint16_t* spineOf = scratch.allocArray<uint16_t>(entryCount);
    if (spineOf == nullptr && entryCount != 0) {
      writer.abort();
      return BookStatus::OutOfMemory;
    }
    for (uint16_t i = 0; i < entryCount; ++i) spineOf[i] = kNoSpine16;
    for (size_t s = spineUsed; s > 0; --s) {  // reverse so the FIRST use wins
      const uint16_t e = spineEntryIdx[s - 1];
      if (e != kNoEntry16) spineOf[e] = static_cast<uint16_t>(s - 1);
    }
    for (uint16_t i = 0; i < entryCount; ++i) {
      HashRec rec;
      rec.nameHash = entries[i].nameHash;
      rec.spineIndex = spineOf[i];
      rec.reserved = 0;
      if (!writer.write(&rec, sizeof(rec))) break;
    }
    scratch.release(marked);
  }

  // Section 7: metadata strings + CSS entry indexes.
  const uint32_t metaOff = writer.offset();
  writer.writeStr(meta->title);
  writer.writeStr(meta->author);
  writer.writeStr(meta->language);
  writer.writeStr(meta->identifier);
  for (size_t i = 0; i < cssUsed; ++i) {
    writer.write(&cssEntryIdx[i], sizeof(uint16_t));
  }
  const uint32_t metaSize = writer.offset() - metaOff;

  // Section 8: the compacted stylesheet. Parse every CSS entry ONCE here and
  // store the resulting POD CssRule array, so open() loads rules resident and
  // never re-parses CSS from the container -- the parse was the only source of
  // per-open nondeterminism in the layout generation hash. The builder's rule
  // array lives in scratch (~14 KB worst case at kMaxRules); each sheet's
  // inflate stream uses the parse arena and is released between sheets.
  const uint32_t cssRulesOff = writer.offset();
  uint32_t cssRuleCount = 0;
  uint32_t cssContentHash = 0;
  {
    const size_t marked = scratch.mark();
    CssStylesheetBuilder cssBuilder;
    if (cssBuilder.begin(scratch)) {
      for (size_t i = 0; i < cssUsed; ++i) {
        const uint16_t row = cssEntryIdx[i];
        if (row >= entryCount) continue;
        const ZipEntry cssZip = toZipEntry(entries[row]);
        const size_t innerMark = parse.mark();
        cssBuilder.addSheet(source, cssZip, parse);
        parse.release(innerMark);
      }
      const CssStylesheet sheet = cssBuilder.finish();
      cssRuleCount = sheet.ruleCount;
      cssContentHash = sheet.contentHash;
      if (sheet.ruleCount > 0) {
        writer.write(sheet.rules, static_cast<uint32_t>(sheet.ruleCount) * sizeof(CssRule));
      }
    }
    scratch.release(marked);
  }

  Footer footer;
  footer.version = kCatalogVersion;
  footer.fingerprint = fingerprint;
  footer.entryCount = entryCount;
  footer.spineCount = static_cast<uint32_t>(spineUsed);
  footer.tocCount = static_cast<uint32_t>(tocUsed);
  footer.cssCount = static_cast<uint32_t>(cssUsed);
  footer.namePoolOff = namePoolOff;
  footer.namePoolSize = namePoolSize;
  footer.titlePoolOff = titlePoolOff;
  footer.titlePoolSize = titlePoolSize;
  footer.entryRecordsOff = entryRecordsOff;
  footer.spineRecordsOff = spineRecordsOff;
  footer.tocRecordsOff = tocRecordsOff;
  footer.hashTableOff = hashTableOff;
  footer.metaOff = metaOff;
  footer.metaSize = metaSize;
  footer.coverEntryIndex = coverEntryIdx;
  footer.cssRulesOff = cssRulesOff;
  footer.cssRuleCount = cssRuleCount;
  footer.cssContentHash = cssContentHash;
  footer.magic = kMagic;
  writer.write(&footer, sizeof(footer));

  if (!writer.ok()) {
    writer.abort();
    return BookStatus::IoError;
  }
  return writer.end() ? BookStatus::Ok : BookStatus::IoError;
}

// --- open + resident-table access -----------------------------------------------

namespace {

BookStatus readFooter(CacheStorage& cache, Footer* out) {
  const int64_t fileSize = cache.fileSize(BookCatalog::kCatalogName);
  if (fileSize < static_cast<int64_t>(sizeof(Footer))) return BookStatus::NotFound;
  if (!readCacheFully(cache, static_cast<uint32_t>(fileSize - sizeof(Footer)), out,
                      sizeof(Footer))) {
    return BookStatus::IoError;
  }
  if (out->magic != kMagic || out->version != kCatalogVersion) return BookStatus::Stale;
  if (out->entryCount > 0xFFFFu || out->spineCount > 0xFFFEu) return BookStatus::Stale;
  return BookStatus::Ok;
}

}  // namespace

BookStatus BookCatalog::residentBytes(CacheStorage& cache, size_t* out) {
  Footer footer;
  const BookStatus st = readFooter(cache, &footer);
  if (st != BookStatus::Ok) return st;
  *out = footer.spineCount * sizeof(SpineRec) + footer.entryCount * sizeof(HashRec) +
         footer.metaSize + footer.cssCount * sizeof(ZipEntry) +
         footer.cssRuleCount * sizeof(CssRule) + 256 /* alignment slack */;
  return BookStatus::Ok;
}

BookStatus BookCatalog::open(BookSource& source, CacheStorage& cache, Arena& bookArena,
                             Arena& scratch) {
  cache_ = nullptr;

  Footer footer;
  BookStatus st = readFooter(cache, &footer);
  if (st != BookStatus::Ok) return st;

  uint32_t fingerprint = 0;
  st = fingerprintContainer(source, scratch, &fingerprint);
  if (st != BookStatus::Ok) return st;
  if (fingerprint != footer.fingerprint) return BookStatus::Stale;

  // Resident tables.
  void* spineBuf = bookArena.alloc(footer.spineCount * sizeof(SpineRec), alignof(SpineRec));
  void* hashBuf = bookArena.alloc(footer.entryCount * sizeof(HashRec), alignof(HashRec));
  char* metaBuf = static_cast<char*>(bookArena.alloc(footer.metaSize + 1, 1));
  if ((spineBuf == nullptr && footer.spineCount != 0) ||
      (hashBuf == nullptr && footer.entryCount != 0) || metaBuf == nullptr) {
    return BookStatus::OutOfMemory;
  }
  if (footer.spineCount != 0 &&
      !readCacheFully(cache, footer.spineRecordsOff, spineBuf,
                      footer.spineCount * sizeof(SpineRec))) {
    return BookStatus::IoError;
  }
  if (footer.entryCount != 0 &&
      !readCacheFully(cache, footer.hashTableOff, hashBuf,
                      footer.entryCount * sizeof(HashRec))) {
    return BookStatus::IoError;
  }
  if (footer.metaSize != 0 &&
      !readCacheFully(cache, footer.metaOff, metaBuf, footer.metaSize)) {
    return BookStatus::IoError;
  }
  metaBuf[footer.metaSize] = '\0';  // backstop for a torn string section

  // Metadata: four NUL-terminated strings, then cssCount u16 entry indexes.
  const char* fields[4] = {"", "", "", ""};
  uint32_t pos = 0;
  for (int f = 0; f < 4; ++f) {
    if (pos >= footer.metaSize) return BookStatus::Stale;
    fields[f] = metaBuf + pos;
    pos += static_cast<uint32_t>(strlen(metaBuf + pos)) + 1;
  }
  if (pos + footer.cssCount * sizeof(uint16_t) > footer.metaSize) return BookStatus::Stale;
  metadata_.title = fields[0];
  metadata_.author = fields[1];
  metadata_.language = fields[2];
  metadata_.identifier = fields[3];

  spineRecs_ = static_cast<const SpineRec*>(spineBuf);
  hashRecs_ = static_cast<const HashRec*>(hashBuf);
  spineCount_ = footer.spineCount;
  tocCount_ = footer.tocCount;
  entryCount_ = footer.entryCount;
  namePoolOff_ = footer.namePoolOff;
  titlePoolOff_ = footer.titlePoolOff;
  entryRecordsOff_ = footer.entryRecordsOff;
  tocRecordsOff_ = footer.tocRecordsOff;
  cache_ = &cache;

  // CSS + cover entries, resolved once so the stylesheet build and cover
  // extraction never touch the index file again.
  cssCount_ = 0;
  cssEntries_ = footer.cssCount > 0 ? bookArena.allocArray<ZipEntry>(footer.cssCount) : nullptr;
  if (footer.cssCount > 0 && cssEntries_ == nullptr) {
    cache_ = nullptr;
    return BookStatus::OutOfMemory;
  }
  for (uint32_t i = 0; i < footer.cssCount; ++i) {
    uint16_t idx = 0;
    memcpy(&idx, metaBuf + pos + i * sizeof(uint16_t), sizeof(uint16_t));
    if (readEntryRecord(idx, &cssEntries_[cssCount_]) == BookStatus::Ok) ++cssCount_;
  }
  hasCover_ = footer.coverEntryIndex != kNoEntry32 &&
              readEntryRecord(footer.coverEntryIndex, &coverEntry_) == BookStatus::Ok;

  // Stored stylesheet: load the compacted rule array resident. Parsed once at
  // build(), so it is byte-identical every open -- no CSS re-parse, and the
  // content hash the layout generation depends on never drifts.
  sheet_ = CssStylesheet{};
  if (footer.cssRuleCount > 0) {
    CssRule* rules = bookArena.allocArray<CssRule>(footer.cssRuleCount);
    if (rules == nullptr) {
      cache_ = nullptr;
      return BookStatus::OutOfMemory;
    }
    if (!readCacheFully(cache, footer.cssRulesOff, rules,
                        footer.cssRuleCount * sizeof(CssRule))) {
      cache_ = nullptr;
      return BookStatus::IoError;
    }
    sheet_.rules = rules;
    sheet_.ruleCount = static_cast<uint16_t>(footer.cssRuleCount);
    sheet_.contentHash = footer.cssContentHash;
  }

  zipView_.owner_ = this;
  return BookStatus::Ok;
}

uint32_t BookCatalog::spineSize(const size_t index) const {
  return index < spineCount_ ? spineRecs_[index].uncompressedSize : 0;
}

BookStatus BookCatalog::spineEntry(const size_t index, ZipEntry* out) const {
  if (!isOpen() || index >= spineCount_) return BookStatus::NotFound;
  const SpineRec& rec = spineRecs_[index];
  if (rec.entryIndex == kNoEntry16) return BookStatus::NotFound;
  return readEntryRecord(rec.entryIndex, out);
}

BookStatus BookCatalog::spineHref(const size_t index, char* buf, const size_t cap) const {
  if (!isOpen() || index >= spineCount_) return BookStatus::NotFound;
  return readPoolString(namePoolOff_, spineRecs_[index].nameOffset, buf, cap);
}

int BookCatalog::spineIndexForHref(const char* href) const {
  if (!isOpen() || href == nullptr || href[0] == '\0') return -1;
  const int row = hashRowFor(ZipCatalog::hashPath(href));
  if (row < 0 || hashRecs_[row].spineIndex == kNoSpine16) return -1;
  return hashRecs_[row].spineIndex;
}

BookStatus BookCatalog::tocItem(const size_t index, TocItem* out, char* titleBuf,
                                const size_t titleCap, char* fragmentBuf,
                                const size_t fragmentCap) const {
  if (!isOpen() || index >= tocCount_) return BookStatus::NotFound;
  TocRec rec;
  if (!readCacheFully(*cache_, tocRecordsOff_ + static_cast<uint32_t>(index) * sizeof(TocRec),
                      &rec, sizeof(rec))) {
    return BookStatus::IoError;
  }
  const BookStatus st = readPoolString(titlePoolOff_, rec.titleOffset, titleBuf, titleCap);
  if (st != BookStatus::Ok) return st;
  out->spineIndex = rec.spineIndex == kNoSpine16 ? -1 : rec.spineIndex;
  out->depth = rec.depth;
  out->hasFragment = rec.fragmentOffset != kNoOffset;
  if (out->hasFragment) {
    const BookStatus fs =
        readPoolString(titlePoolOff_, rec.fragmentOffset, fragmentBuf, fragmentCap);
    if (fs != BookStatus::Ok) return fs;
  } else if (fragmentCap > 0) {
    fragmentBuf[0] = '\0';
  }
  return BookStatus::Ok;
}

int BookCatalog::tocIndexForSpine(const int spineIndex) const {
  if (!isOpen()) return -1;
  // The chapter's title is the last TOC entry at or before this spine item --
  // scanned over the fixed-size records in small chunks, no title reads.
  constexpr uint32_t kChunk = 20;  // 240 B on the stack
  TocRec recs[kChunk];
  int best = -1;
  int bestSpine = -1;
  for (uint32_t base = 0; base < tocCount_; base += kChunk) {
    const uint32_t n = tocCount_ - base < kChunk ? tocCount_ - base : kChunk;
    if (!readCacheFully(*cache_, tocRecordsOff_ + base * sizeof(TocRec), recs,
                        n * sizeof(TocRec))) {
      return best;
    }
    for (uint32_t i = 0; i < n; ++i) {
      if (recs[i].spineIndex == kNoSpine16) continue;
      const int s = recs[i].spineIndex;
      if (s > spineIndex) continue;
      if (s >= bestSpine) {
        bestSpine = s;
        best = static_cast<int>(base + i);
      }
    }
  }
  return best;
}

bool BookCatalog::coverEntry(ZipEntry* out) const {
  if (!hasCover_) return false;
  *out = coverEntry_;
  return true;
}

int BookCatalog::hashRowFor(const uint32_t nameHash) const {
  int lo = 0;
  int hi = static_cast<int>(entryCount_) - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    if (hashRecs_[mid].nameHash < nameHash) {
      lo = mid + 1;
    } else if (hashRecs_[mid].nameHash > nameHash) {
      hi = mid - 1;
    } else {
      int row = mid;
      while (row > 0 && hashRecs_[row - 1].nameHash == nameHash) --row;
      return row;
    }
  }
  return -1;
}

BookStatus BookCatalog::readEntryRecord(const uint32_t entryIndex, ZipEntry* out) const {
  if (!isOpen() || entryIndex >= entryCount_) return BookStatus::NotFound;
  EntryRec rec;
  if (!readCacheFully(*cache_, entryRecordsOff_ + entryIndex * sizeof(EntryRec), &rec,
                      sizeof(rec))) {
    return BookStatus::IoError;
  }
  out->name = "";  // the string lives only on SD; nothing downstream reads it
  out->nameHash = hashRecs_[entryIndex].nameHash;
  out->compressedSize = rec.compressedSize;
  out->uncompressedSize = rec.uncompressedSize;
  out->localHeaderOffset = rec.localHeaderOffset;
  out->method = rec.method;
  return BookStatus::Ok;
}

BookStatus BookCatalog::readPoolString(const uint32_t poolOffset, const uint32_t stringOffset,
                                       char* buf, const size_t cap) const {
  if (!isOpen() || buf == nullptr || cap == 0) return BookStatus::NotFound;
  // readAt may return short; keep reading until the terminator shows up, the
  // buffer fills (truncate -- titles may exceed a caller's display buffer), or
  // the file ends.
  uint32_t got = 0;
  const uint32_t want = static_cast<uint32_t>(cap - 1);
  while (got < want) {
    const int32_t n =
        cache_->readAt(kCatalogName, poolOffset + stringOffset + got, buf + got, want - got);
    if (n <= 0) break;
    if (memchr(buf + got, '\0', static_cast<size_t>(n)) != nullptr) return BookStatus::Ok;
    got += static_cast<uint32_t>(n);
  }
  if (got == 0) return BookStatus::IoError;
  buf[got] = '\0';
  return BookStatus::Ok;
}

const ZipEntry* BookCatalog::CatalogZipView::find(const char* path) const {
  if (path == nullptr) return nullptr;
  return findByHash(hashPath(path));
}

const ZipEntry* BookCatalog::CatalogZipView::findByHash(const uint32_t nameHash) const {
  if (owner_ == nullptr || !owner_->isOpen()) return nullptr;
  const int row = owner_->hashRowFor(nameHash);
  if (row < 0) return nullptr;
  if (owner_->readEntryRecord(static_cast<uint32_t>(row), &slot_) != BookStatus::Ok) {
    return nullptr;
  }
  return &slot_;
}

}  // namespace book
}  // namespace freeink
