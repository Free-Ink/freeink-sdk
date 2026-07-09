// Host-side unit tests for BookCatalog (the SD-backed container catalog).
// Parity against the in-RAM Book on the small fixtures and on a generated
// 1,700-spine omnibus, plus the memory-ceiling assertions that are the whole
// point: resident tables stay under ~45 KB where Book needs hundreds of KB.
// Run with test/host/run.sh.

#include <BookCatalog.h>
#include <FreeInkBook.h>
#include <layout/ChapterLayout.h>

#include <cstdio>
#include <cstring>

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checksRun;                                                           \
    if (!(cond)) {                                                         \
      ++checksFailed;                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    ++checksRun;                                                                        \
    const auto va = (a);                                                                \
    const auto vb = (b);                                                                \
    if (!(va == vb)) {                                                                  \
      ++checksFailed;                                                                   \
      std::printf("FAIL %s:%d  %s == %s  (%ld != %ld)\n", __FILE__, __LINE__, #a, #b,   \
                  static_cast<long>(va), static_cast<long>(vb));                        \
    }                                                                                   \
  } while (0)

#define CHECK_STREQ(a, b)                                                               \
  do {                                                                                  \
    ++checksRun;                                                                        \
    const char* va = (a);                                                               \
    const char* vb = (b);                                                               \
    if (va == nullptr || vb == nullptr || std::strcmp(va, vb) != 0) {                   \
      ++checksFailed;                                                                   \
      std::printf("FAIL %s:%d  %s == %s  (\"%s\" != \"%s\")\n", __FILE__, __LINE__, #a, \
                  #b, va != nullptr ? va : "(null)", vb != nullptr ? vb : "(null)");    \
    }                                                                                   \
  } while (0)

using namespace freeink::book;

class HostFileSource : public BookSource {
 public:
  ~HostFileSource() override {
    if (file_ != nullptr) std::fclose(file_);
  }
  bool open(const char* path) {
    if (file_ != nullptr) std::fclose(file_);
    file_ = std::fopen(path, "rb");
    if (file_ == nullptr) return false;
    std::fseek(file_, 0, SEEK_END);
    size_ = static_cast<uint64_t>(std::ftell(file_));
    return true;
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) return -1;
    return static_cast<int32_t>(std::fread(dst, 1, len, file_));
  }
  uint64_t size() const override { return size_; }

 private:
  FILE* file_ = nullptr;
  uint64_t size_ = 0;
};

class HostCacheStorage : public CacheStorage {
 public:
  explicit HostCacheStorage(const char* dir) : dir_(dir) {}

  bool exists(const char* name) override {
    FILE* f = std::fopen(path(name), "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
  }
  bool remove(const char* name) override { return ::remove(path(name)) == 0; }
  int64_t fileSize(const char* name) override {
    FILE* f = std::fopen(path(name), "rb");
    if (f == nullptr) return -1;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);
    return size;
  }
  int32_t readAt(const char* name, uint32_t offset, void* dst, uint32_t len) override {
    FILE* f = std::fopen(path(name), "rb");
    if (f == nullptr) return -1;
    std::fseek(f, static_cast<long>(offset), SEEK_SET);
    const size_t n = std::fread(dst, 1, len, f);
    std::fclose(f);
    return static_cast<int32_t>(n);
  }
  bool beginWrite(const char* name) override {
    std::snprintf(writePath_, sizeof(writePath_), "%s/%s", dir_, name);
    writeFile_ = std::fopen(writePath_, "wb");
    return writeFile_ != nullptr;
  }
  bool write(const void* data, uint32_t len) override {
    return writeFile_ != nullptr && std::fwrite(data, 1, len, writeFile_) == len;
  }
  bool endWrite() override {
    if (writeFile_ == nullptr) return false;
    const bool ok = std::fclose(writeFile_) == 0;
    writeFile_ = nullptr;
    return ok;
  }

 private:
  const char* path(const char* name) {
    std::snprintf(pathBuf_, sizeof(pathBuf_), "%s/%s", dir_, name);
    return pathBuf_;
  }
  const char* dir_;
  char pathBuf_[1024];
  char writePath_[1024];
  FILE* writeFile_ = nullptr;
};

// Big host arenas; the assertions on highWater() are what enforce the device
// budgets, not the buffer sizes.
uint8_t bookBuf[768 * 1024];
uint8_t catalogBuf[128 * 1024];
uint8_t scratchBuf[512 * 1024];
uint8_t parseBuf[256 * 1024];

char fixturePath[1024];
const char* fixturesDir = nullptr;
const char* cacheDir = nullptr;

const char* fixture(const char* name) {
  std::snprintf(fixturePath, sizeof(fixturePath), "%s/%s", fixturesDir, name);
  return fixturePath;
}

// Spine index the in-RAM book resolves a TOC href to (strcmp, like the app).
int bookSpineForHref(const Book& book, const char* href) {
  for (size_t s = 0; s < book.spineCount(); ++s) {
    const ManifestItem* item = book.spineItem(s);
    if (item != nullptr && std::strcmp(item->href, href) == 0) return static_cast<int>(s);
  }
  return -1;
}

// Full parity sweep between an open Book and an open BookCatalog.
void checkParity(const Book& book, const BookCatalog& catalog) {
  CHECK_STREQ(catalog.metadata().title, book.metadata().title);
  CHECK_STREQ(catalog.metadata().author, book.metadata().author);
  CHECK_STREQ(catalog.metadata().language, book.metadata().language);
  CHECK_STREQ(catalog.metadata().identifier, book.metadata().identifier);
  CHECK_EQ(catalog.spineCount(), book.spineCount());
  CHECK_EQ(catalog.tocCount(), book.tocCount());

  char href[512];
  for (size_t s = 0; s < book.spineCount(); ++s) {
    const ManifestItem* item = book.spineItem(s);
    CHECK_EQ(static_cast<int>(catalog.spineHref(s, href, sizeof(href))),
             static_cast<int>(BookStatus::Ok));
    CHECK_STREQ(href, item->href);

    const ZipEntry* zipEntry = book.zip().find(item->href);
    ZipEntry catEntry;
    if (zipEntry == nullptr) {
      CHECK(catalog.spineEntry(s, &catEntry) != BookStatus::Ok);
      CHECK_EQ(catalog.spineSize(s), 0u);
      continue;
    }
    CHECK_EQ(static_cast<int>(catalog.spineEntry(s, &catEntry)),
             static_cast<int>(BookStatus::Ok));
    CHECK_EQ(catEntry.localHeaderOffset, zipEntry->localHeaderOffset);
    CHECK_EQ(catEntry.compressedSize, zipEntry->compressedSize);
    CHECK_EQ(catEntry.uncompressedSize, zipEntry->uncompressedSize);
    CHECK_EQ(catEntry.method, zipEntry->method);
    CHECK_EQ(catEntry.nameHash, zipEntry->nameHash);
    CHECK_EQ(catalog.spineSize(s), zipEntry->uncompressedSize);
    CHECK_EQ(catalog.spineIndexForHref(item->href), bookSpineForHref(book, item->href));
  }

  char title[256];
  char fragment[256];
  for (size_t t = 0; t < book.tocCount(); ++t) {
    const TocEntry* e = book.tocEntry(t);
    BookCatalog::TocItem item;
    CHECK_EQ(static_cast<int>(
                 catalog.tocItem(t, &item, title, sizeof(title), fragment, sizeof(fragment))),
             static_cast<int>(BookStatus::Ok));
    CHECK_STREQ(title, e->title);
    CHECK_EQ(item.depth, e->depth);
    CHECK_EQ(item.hasFragment, e->fragment != nullptr);
    if (e->fragment != nullptr) CHECK_STREQ(fragment, e->fragment);
    CHECK_EQ(item.spineIndex, bookSpineForHref(book, e->href));
  }

  // zip() lookup parity for every manifest item (images, css, chapters).
  for (size_t m = 0; m < book.manifestCount(); ++m) {
    const ManifestItem* item = book.manifestItem(m);
    const ZipEntry* zipEntry = book.zip().find(item->href);
    const ZipEntry* catEntry = catalog.zip().find(item->href);
    CHECK_EQ(catEntry != nullptr, zipEntry != nullptr);
    if (zipEntry != nullptr && catEntry != nullptr) {
      CHECK_EQ(catEntry->localHeaderOffset, zipEntry->localHeaderOffset);
      CHECK_EQ(catEntry->compressedSize, zipEntry->compressedSize);
      CHECK_EQ(catEntry->uncompressedSize, zipEntry->uncompressedSize);
      CHECK_EQ(catEntry->method, zipEntry->method);
    }
  }
  CHECK(catalog.zip().find("no/such/entry.xhtml") == nullptr);

  // Cover parity: the manifest item flagged isCoverImage must match.
  const ZipEntry* bookCover = nullptr;
  for (size_t m = 0; m < book.manifestCount(); ++m) {
    const ManifestItem* item = book.manifestItem(m);
    if (item->isCoverImage) {
      bookCover = book.zip().find(item->href);
      break;
    }
  }
  ZipEntry catCover;
  CHECK_EQ(catalog.coverEntry(&catCover), bookCover != nullptr);
  if (bookCover != nullptr && catalog.coverEntry(&catCover)) {
    CHECK_EQ(catCover.localHeaderOffset, bookCover->localHeaderOffset);
    CHECK_EQ(catCover.uncompressedSize, bookCover->uncompressedSize);
  }

  // CSS entries: every text/css manifest item present in the zip.
  size_t cssExpected = 0;
  for (size_t m = 0; m < book.manifestCount(); ++m) {
    const ManifestItem* item = book.manifestItem(m);
    if (std::strcmp(item->mediaType, "text/css") == 0 && book.zip().find(item->href) != nullptr) {
      ++cssExpected;
    }
  }
  CHECK_EQ(catalog.cssCount(), cssExpected);

  // tocIndexForSpine parity against the app's reference loop.
  for (size_t s = 0; s < book.spineCount(); ++s) {
    int best = -1;
    int bestSpine = -1;
    for (size_t t = 0; t < book.tocCount(); ++t) {
      const int ts = bookSpineForHref(book, book.tocEntry(t)->href);
      if (ts < 0 || ts > static_cast<int>(s)) continue;
      if (ts >= bestSpine) {
        bestSpine = ts;
        best = static_cast<int>(t);
      }
    }
    CHECK_EQ(catalog.tocIndexForSpine(static_cast<int>(s)), best);
    if (checksFailed > 50) return;  // one systematic mismatch would flood the log
  }
}

void testCatalogParity(const char* name) {
  HostFileSource source;
  CHECK(source.open(fixture(name)));
  HostCacheStorage cache(cacheDir);
  cache.remove(BookCatalog::kCatalogName);

  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, 256 * 1024);
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  Arena buildScratch(scratchBuf + 256 * 1024, 256 * 1024);
  Arena parseArena(parseBuf, sizeof(parseBuf));
  CHECK_EQ(static_cast<int>(BookCatalog::build(source, cache, buildScratch, &parseArena)),
           static_cast<int>(BookStatus::Ok));

  size_t resident = 0;
  CHECK_EQ(static_cast<int>(BookCatalog::residentBytes(cache, &resident)),
           static_cast<int>(BookStatus::Ok));

  Arena catalogArena(catalogBuf, sizeof(catalogBuf));
  BookCatalog catalog;
  CHECK_EQ(static_cast<int>(catalog.open(source, cache, catalogArena, scratch)),
           static_cast<int>(BookStatus::Ok));
  CHECK(catalogArena.used() <= resident);

  checkParity(book, catalog);
  std::printf("  %-14s catalog resident %zu B (Book arena %zu B)\n", name,
              catalogArena.used(), bookArena.highWater());
}

// A chapter must lay out identically whether the entry/href come from Book or
// from the catalog (same bytes, same probe results through the virtual find).
void testCatalogLayout() {
  HostFileSource source;
  CHECK(source.open(fixture("minimal.epub")));
  HostCacheStorage cache(cacheDir);
  cache.remove(BookCatalog::kCatalogName);

  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, 256 * 1024);
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));
  Arena buildScratch(scratchBuf + 256 * 1024, 256 * 1024);
  CHECK_EQ(static_cast<int>(BookCatalog::build(source, cache, buildScratch)),
           static_cast<int>(BookStatus::Ok));
  Arena catalogArena(catalogBuf, sizeof(catalogBuf));
  BookCatalog catalog;
  CHECK_EQ(static_cast<int>(catalog.open(source, cache, catalogArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  class HashSink : public PageSink {
   public:
    bool onPage(const Page& page) override {
      ++pages;
      for (uint16_t r = 0; r < page.runCount; ++r) {
        hash = static_cast<uint32_t>(hash * 31u + page.runs[r].len);
        for (uint16_t c = 0; c < page.runs[r].len; ++c) {
          hash = static_cast<uint32_t>(hash * 131u + static_cast<uint8_t>(page.runs[r].text[c]));
        }
      }
      return true;
    }
    uint32_t pages = 0;
    uint32_t hash = 5381;
  };

  class FixedFont : public BookFont {
   public:
    int16_t advance(uint32_t, uint16_t sizePx, uint8_t) override {
      return static_cast<int16_t>(sizePx / 2);
    }
    int16_t lineHeight(uint16_t sizePx) override { return static_cast<int16_t>(sizePx * 5 / 4); }
    int16_t ascent(uint16_t sizePx) override { return static_cast<int16_t>(sizePx); }
  };

  FixedFont font;
  LayoutParams params;
  params.font = &font;

  // Spine 3 ("ch3") of the minimal fixture contains the images -- the chapter
  // that exercises the probe path through zip().findByHash.
  const size_t spineIdx = 3;
  const ManifestItem* item = book.spineItem(spineIdx);
  const ZipEntry* bookEntry = book.zip().find(item->href);
  CHECK(bookEntry != nullptr);

  HashSink viaBook;
  {
    Arena layout(scratchBuf + 256 * 1024, 256 * 1024);
    CHECK_EQ(static_cast<int>(ChapterLayout::layout(source, book.zip(), *bookEntry, item->href,
                                                    params, layout, viaBook, nullptr, nullptr)),
             static_cast<int>(BookStatus::Ok));
  }

  ZipEntry catEntry;
  char href[512];
  CHECK_EQ(static_cast<int>(catalog.spineEntry(spineIdx, &catEntry)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(static_cast<int>(catalog.spineHref(spineIdx, href, sizeof(href))),
           static_cast<int>(BookStatus::Ok));
  HashSink viaCatalog;
  {
    Arena layout(scratchBuf + 256 * 1024, 256 * 1024);
    CHECK_EQ(static_cast<int>(ChapterLayout::layout(source, catalog.zip(), catEntry, href,
                                                    params, layout, viaCatalog, nullptr,
                                                    nullptr)),
             static_cast<int>(BookStatus::Ok));
  }
  CHECK_EQ(viaCatalog.pages, viaBook.pages);
  CHECK_EQ(viaCatalog.hash, viaBook.hash);
}

void testStaleAndReopen() {
  HostFileSource source;
  CHECK(source.open(fixture("minimal.epub")));
  HostCacheStorage cache(cacheDir);
  cache.remove(BookCatalog::kCatalogName);

  Arena scratch(scratchBuf, 256 * 1024);
  Arena buildScratch(scratchBuf + 256 * 1024, 256 * 1024);
  CHECK_EQ(static_cast<int>(BookCatalog::build(source, cache, buildScratch)),
           static_cast<int>(BookStatus::Ok));

  // Reopen fast path: a second open() against the same container succeeds
  // with no rebuild.
  {
    Arena catalogArena(catalogBuf, sizeof(catalogBuf));
    BookCatalog catalog;
    CHECK_EQ(static_cast<int>(catalog.open(source, cache, catalogArena, scratch)),
             static_cast<int>(BookStatus::Ok));
  }
  {
    Arena catalogArena(catalogBuf, sizeof(catalogBuf));
    BookCatalog catalog;
    CHECK_EQ(static_cast<int>(catalog.open(source, cache, catalogArena, scratch)),
             static_cast<int>(BookStatus::Ok));
  }

  // A different container (changed central directory) must read as Stale.
  HostFileSource other;
  CHECK(other.open(fixture("ncxonly.epub")));
  {
    Arena catalogArena(catalogBuf, sizeof(catalogBuf));
    BookCatalog catalog;
    CHECK_EQ(static_cast<int>(catalog.open(other, cache, catalogArena, scratch)),
             static_cast<int>(BookStatus::Stale));
  }

  // Missing catalog reads as NotFound (the caller's "build it" signal).
  cache.remove(BookCatalog::kCatalogName);
  {
    Arena catalogArena(catalogBuf, sizeof(catalogBuf));
    BookCatalog catalog;
    CHECK_EQ(static_cast<int>(catalog.open(source, cache, catalogArena, scratch)),
             static_cast<int>(BookStatus::NotFound));
    size_t resident = 0;
    CHECK_EQ(static_cast<int>(BookCatalog::residentBytes(cache, &resident)),
             static_cast<int>(BookStatus::NotFound));
  }
}

// The reason this class exists: an omnibus whose in-RAM catalog needs
// hundreds of KB must open under tight, device-real ceilings.
void testOmnibus() {
  HostFileSource source;
  CHECK(source.open(fixture("omnibus.epub")));
  HostCacheStorage cache(cacheDir);
  cache.remove(BookCatalog::kCatalogName);

  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, 256 * 1024);
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(book.spineCount(), 1700u);
  CHECK_EQ(book.tocCount(), 1700u);

  // Build under device-shaped arenas: the record tables and the parse stream
  // split exactly like a chapter build. These ceilings are the device budget.
  Arena buildScratch(scratchBuf + 256 * 1024, 96 * 1024);
  Arena parseArena(parseBuf, 56 * 1024);
  CHECK_EQ(static_cast<int>(BookCatalog::build(source, cache, buildScratch, &parseArena)),
           static_cast<int>(BookStatus::Ok));
  std::printf("  omnibus build: scratch high water %zu B, parse high water %zu B\n",
              buildScratch.highWater(), parseArena.highWater());
  // The stored-stylesheet section parses CSS into a CssStylesheetBuilder whose
  // rule array (kMaxRules) is a transient ~16 KB draw from the build scratch,
  // so the omnibus peak sits a little above the pre-v2 80 KB. Still well under
  // the 96 KB ideal records arena BookPaginator::buildCatalog allocates.
  CHECK(buildScratch.highWater() < 88 * 1024);
  CHECK(parseArena.highWater() < 52 * 1024);

  size_t resident = 0;
  CHECK_EQ(static_cast<int>(BookCatalog::residentBytes(cache, &resident)),
           static_cast<int>(BookStatus::Ok));
  CHECK(resident < 46 * 1024);

  Arena catalogArena(catalogBuf, resident);
  BookCatalog catalog;
  CHECK_EQ(static_cast<int>(catalog.open(source, cache, catalogArena, scratch)),
           static_cast<int>(BookStatus::Ok));
  std::printf("  omnibus resident: %zu B for %zu spines (Book arena needed %zu B)\n",
              catalogArena.highWater(), catalog.spineCount(), bookArena.highWater());
  CHECK(catalogArena.highWater() < 45 * 1024);

  checkParity(book, catalog);
}

// Parses every text/css entry the Book resolves in RAM into a stylesheet, so
// the catalog's stored stylesheet can be compared rule for rule.
uint16_t bookStylesheetRuleCount(Book& book, BookSource& source, uint32_t* hashOut) {
  Arena sheetArena(scratchBuf + 384 * 1024, 64 * 1024);
  Arena cssScratch(parseBuf, 128 * 1024);
  CssStylesheetBuilder builder;
  CHECK(builder.begin(sheetArena));
  for (size_t m = 0; m < book.manifestCount(); ++m) {
    const ManifestItem* item = book.manifestItem(m);
    if (item == nullptr || item->mediaType == nullptr ||
        std::strcmp(item->mediaType, "text/css") != 0) {
      continue;
    }
    if (const ZipEntry* e = book.zip().find(item->href)) {
      builder.addSheet(source, *e, cssScratch);
    }
  }
  const CssStylesheet sheet = builder.finish();
  if (hashOut != nullptr) *hashOut = sheet.contentHash;
  return sheet.ruleCount;
}

// Regression: opening a catalog FRESH from disk must yield the same parsed
// stylesheet Book produces in RAM -- non-zero, and byte-stable across
// independent opens. Before the stored-stylesheet fix, from-disk opens parsed
// 0 rules (dropping all book styling and destabilizing the layout generation
// hash), while the post-build open still saw the rules.
void testStoredStylesheet() {
  HostFileSource source;
  CHECK(source.open(fixture("minimal.epub")));
  HostCacheStorage cache(cacheDir);
  cache.remove(BookCatalog::kCatalogName);

  // Reference: what Book's in-RAM CSS build produces.
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, 256 * 1024);
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));
  uint32_t bookHash = 0;
  const uint16_t bookRules = bookStylesheetRuleCount(book, source, &bookHash);
  CHECK(bookRules > 0);  // the fixture stylesheet has real rules

  Arena buildScratch(scratchBuf + 256 * 1024, 128 * 1024);
  Arena parseArena(parseBuf, 128 * 1024);
  CHECK_EQ(static_cast<int>(BookCatalog::build(source, cache, buildScratch, &parseArena)),
           static_cast<int>(BookStatus::Ok));

  // Two INDEPENDENT from-disk opens (fresh BookCatalog + fresh source each) --
  // no post-build state carried over. Both must match Book exactly.
  uint32_t firstHash = 0;
  uint16_t firstRules = 0;
  for (int pass = 0; pass < 2; ++pass) {
    HostFileSource diskSource;
    CHECK(diskSource.open(fixture("minimal.epub")));
    Arena catalogArena(catalogBuf, sizeof(catalogBuf));
    BookCatalog catalog;
    CHECK_EQ(static_cast<int>(catalog.open(diskSource, cache, catalogArena, scratch)),
             static_cast<int>(BookStatus::Ok));
    const CssStylesheet* sheet = catalog.stylesheet();
    CHECK(sheet != nullptr);
    if (sheet == nullptr) return;
    CHECK(sheet->ruleCount > 0);
    CHECK_EQ(sheet->ruleCount, bookRules);
    CHECK_EQ(sheet->contentHash, bookHash);
    if (pass == 0) {
      firstRules = sheet->ruleCount;
      firstHash = sheet->contentHash;
    } else {
      CHECK_EQ(sheet->ruleCount, firstRules);   // stable across opens
      CHECK_EQ(sheet->contentHash, firstHash);  // -> stable generation hash
    }
  }
  std::printf("  stored stylesheet: %u rules, hash %08x (Book in-RAM %u rules)\n", firstRules,
              firstHash, bookRules);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: %s <fixtures-build-dir> <cache-dir>\n", argv[0]);
    return 2;
  }
  fixturesDir = argv[1];
  cacheDir = argv[2];

  testCatalogParity("minimal.epub");
  testCatalogParity("stored.epub");
  testCatalogParity("ncxonly.epub");  // EPUB 2 NCX path
  testCatalogLayout();
  testStoredStylesheet();
  testStaleAndReopen();
  testOmnibus();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
