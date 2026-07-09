// fibcheck -- runs a real EPUB through the whole FreeInkBook pipeline on the
// host and reports what happened: container parse, metadata, TOC, CSS rules,
// per-chapter layout status, page counts, font sizes seen (dropcaps and
// heading scales show up here), and arena high-water marks.
//
// Build/run via test/host/fibcheck.sh <book.epub> [hyph-en-us.fibh]

#include <BookCatalog.h>
#include <FreeInkBook.h>
#include <css/Css.h>
#include <layout/ChapterLayout.h>
#include <text/Hyphenator.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace freeink::book;

namespace {

class HostFileSource : public BookSource {
 public:
  ~HostFileSource() override {
    if (file_ != nullptr) fclose(file_);
  }
  bool open(const char* path) {
    file_ = fopen(path, "rb");
    if (file_ == nullptr) return false;
    fseek(file_, 0, SEEK_END);
    size_ = static_cast<uint64_t>(ftell(file_));
    return true;
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) return -1;
    return static_cast<int32_t>(fread(dst, 1, len, file_));
  }
  uint64_t size() const override { return size_; }

 private:
  FILE* file_ = nullptr;
  uint64_t size_ = 0;
};

// Minimal cache-dir adapter for --catalog runs.
class HostCacheStorage : public CacheStorage {
 public:
  explicit HostCacheStorage(const char* dir) : dir_(dir) {}

  bool exists(const char* name) override {
    FILE* f = fopen(path(name), "rb");
    if (f == nullptr) return false;
    fclose(f);
    return true;
  }
  bool remove(const char* name) override { return ::remove(path(name)) == 0; }
  int64_t fileSize(const char* name) override {
    FILE* f = fopen(path(name), "rb");
    if (f == nullptr) return -1;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);
    return size;
  }
  int32_t readAt(const char* name, uint32_t offset, void* dst, uint32_t len) override {
    FILE* f = fopen(path(name), "rb");
    if (f == nullptr) return -1;
    fseek(f, static_cast<long>(offset), SEEK_SET);
    const size_t n = fread(dst, 1, len, f);
    fclose(f);
    return static_cast<int32_t>(n);
  }
  bool beginWrite(const char* name) override {
    snprintf(writePath_, sizeof(writePath_), "%s/%s", dir_, name);
    writeFile_ = fopen(writePath_, "wb");
    return writeFile_ != nullptr;
  }
  bool write(const void* data, uint32_t len) override {
    return writeFile_ != nullptr && fwrite(data, 1, len, writeFile_) == len;
  }
  bool endWrite() override {
    if (writeFile_ == nullptr) return false;
    const bool ok = fclose(writeFile_) == 0;
    writeFile_ = nullptr;
    return ok;
  }

 private:
  const char* path(const char* name) {
    snprintf(pathBuf_, sizeof(pathBuf_), "%s/%s", dir_, name);
    return pathBuf_;
  }
  const char* dir_;
  char pathBuf_[1024];
  char writePath_[1024];
  FILE* writeFile_ = nullptr;
};

// Metrics-only stand-in font (halfwidth advances). Real rendering metrics
// arrive with device fonts; for pipeline validation only proportions matter.
class CheckFont : public BookFont {
 public:
  int16_t advance(uint32_t cp, uint16_t sizePx, uint8_t) override {
    return static_cast<int16_t>(cp < 0x2E80 ? sizePx / 2 : sizePx);  // CJK fullwidth
  }
  int16_t lineHeight(uint16_t sizePx) override { return static_cast<int16_t>(sizePx * 5 / 4); }
  int16_t ascent(uint16_t sizePx) override { return sizePx; }
};

class StatsSink : public PageSink {
 public:
  bool onPage(const Page& page) override {
    ++pages;
    for (uint16_t r = 0; r < page.runCount; ++r) {
      runs++;
      const uint16_t s = page.runs[r].sizePx;
      for (uint32_t i = 0; i < sizeCount; ++i) {
        if (sizes[i] == s) goto seen;
      }
      if (sizeCount < 16) sizes[sizeCount++] = s;
    seen:;
      textBytes += page.runs[r].len;
    }
    return true;
  }
  uint32_t pages = 0;
  uint32_t runs = 0;
  uint64_t textBytes = 0;
  uint16_t sizes[16];
  uint32_t sizeCount = 0;
};

uint8_t bookBuf[768 * 1024];  // webnovel omnibuses: 1800+ zip entries, 1500+ spine items
uint8_t scratchBuf[512 * 1024];
uint8_t parseBuf[512 * 1024];  // backing for --parse=N split-arena runs
uint8_t sheetBuf[128 * 1024];
uint8_t hyphBuf[96 * 1024];

}  // namespace

int main(int argc, char** argv) {
  // --layout=N/--parse=N emulate a constrained device build: the chapter
  // loop below then uses a split arena pair of exactly those byte sizes
  // instead of one huge host arena, so a device OOM reproduces here.
  size_t layoutCap = 0;
  size_t parseCap = 0;
  const char* bookPath = nullptr;
  const char* hyphPath = nullptr;
  const char* catalogDir = nullptr;
  for (int a = 1; a < argc; ++a) {
    if (strncmp(argv[a], "--layout=", 9) == 0) {
      layoutCap = static_cast<size_t>(strtoul(argv[a] + 9, nullptr, 10));
    } else if (strncmp(argv[a], "--parse=", 8) == 0) {
      parseCap = static_cast<size_t>(strtoul(argv[a] + 8, nullptr, 10));
    } else if (strncmp(argv[a], "--catalog=", 10) == 0) {
      catalogDir = argv[a] + 10;
    } else if (bookPath == nullptr) {
      bookPath = argv[a];
    } else {
      hyphPath = argv[a];
    }
  }
  if (bookPath == nullptr) {
    printf("usage: %s [--layout=BYTES --parse=BYTES] [--catalog=CACHE_DIR] <book.epub> "
           "[hyph.fibh]\n", argv[0]);
    return 2;
  }
  const bool split = layoutCap != 0 && parseCap != 0;
  if (split && (layoutCap > sizeof(scratchBuf) || parseCap > sizeof(scratchBuf))) {
    printf("arena caps exceed host buffers\n");
    return 2;
  }

  HostFileSource source;
  if (!source.open(bookPath)) {
    printf("cannot open %s\n", bookPath);
    return 2;
  }

  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));
  Book book;
  const BookStatus status = book.open(source, bookArena, scratch);
  printf("open: %s\n", bookStatusName(status));
  if (status != BookStatus::Ok) return 1;

  printf("title: %s\nauthor: %s\nlanguage: %s\n", book.metadata().title,
         book.metadata().author, book.metadata().language);
  printf("manifest: %zu items, spine: %zu, toc: %zu entries\n", book.manifestCount(),
         book.spineCount(), book.tocCount());
  for (size_t t = 0; t < book.tocCount() && t < 5; ++t) {
    printf("  toc[%zu] '%s' -> %s\n", t, book.tocEntry(t)->title, book.tocEntry(t)->href);
  }

  // --catalog: build + open the SD-backed catalog, verify parity against the
  // in-RAM Book, and report the device-relevant arena numbers.
  static HostCacheStorage cache(catalogDir != nullptr ? catalogDir : ".");
  static BookCatalog catalog;
  static uint8_t catalogArenaBuf[96 * 1024];
  bool useCatalog = false;
  if (catalogDir != nullptr) {
    if (!cache.exists(BookCatalog::kCatalogName)) {
      Arena buildScratch(scratchBuf, 128 * 1024);
      Arena buildParse(parseBuf, 56 * 1024);
      const BookStatus bs = BookCatalog::build(source, cache, buildScratch, &buildParse);
      printf("catalog build: %s (scratch high water %zu B, parse high water %zu B)\n",
             bookStatusName(bs), buildScratch.highWater(), buildParse.highWater());
      if (bs != BookStatus::Ok) return 1;
    } else {
      printf("catalog build: reusing existing index\n");
    }
    size_t resident = 0;
    if (BookCatalog::residentBytes(cache, &resident) != BookStatus::Ok) return 1;
    Arena catalogArena(catalogArenaBuf, resident < sizeof(catalogArenaBuf)
                                            ? resident
                                            : sizeof(catalogArenaBuf));
    const BookStatus cs = catalog.open(source, cache, catalogArena, scratch);
    printf("catalog open: %s (resident %zu B / advertised %zu B, %zu spines, %zu toc)\n",
           bookStatusName(cs), catalogArena.highWater(), resident, catalog.spineCount(),
           catalog.tocCount());
    if (cs != BookStatus::Ok) return 1;

    uint32_t mismatches = 0;
    if (catalog.spineCount() != book.spineCount()) ++mismatches;
    if (catalog.tocCount() != book.tocCount()) ++mismatches;
    if (strcmp(catalog.metadata().title, book.metadata().title) != 0) ++mismatches;
    if (strcmp(catalog.metadata().author, book.metadata().author) != 0) ++mismatches;
    char href[512];
    for (size_t s = 0; s < book.spineCount(); ++s) {
      const ManifestItem* item = book.spineItem(s);
      ZipEntry ce;
      if (catalog.spineHref(s, href, sizeof(href)) != BookStatus::Ok ||
          strcmp(href, item->href) != 0) {
        ++mismatches;
        continue;
      }
      const ZipEntry* ze = book.zip().find(item->href);
      if (ze == nullptr) continue;
      if (catalog.spineEntry(s, &ce) != BookStatus::Ok ||
          ce.localHeaderOffset != ze->localHeaderOffset ||
          ce.compressedSize != ze->compressedSize ||
          ce.uncompressedSize != ze->uncompressedSize || ce.method != ze->method) {
        ++mismatches;
      }
    }
    char title[256];
    char fragment[256];
    for (size_t t = 0; t < book.tocCount(); ++t) {
      const TocEntry* e = book.tocEntry(t);
      BookCatalog::TocItem item;
      if (catalog.tocItem(t, &item, title, sizeof(title), fragment, sizeof(fragment)) !=
              BookStatus::Ok ||
          strcmp(title, e->title) != 0 || item.depth != e->depth ||
          item.hasFragment != (e->fragment != nullptr)) {
        ++mismatches;
      }
    }
    for (size_t m = 0; m < book.manifestCount(); ++m) {
      const ZipEntry* ze = book.zip().find(book.manifestItem(m)->href);
      const ZipEntry* ce = catalog.zip().find(book.manifestItem(m)->href);
      if ((ze != nullptr) != (ce != nullptr)) ++mismatches;
      else if (ze != nullptr && ce != nullptr &&
               (ce->localHeaderOffset != ze->localHeaderOffset ||
                ce->uncompressedSize != ze->uncompressedSize)) {
        ++mismatches;
      }
    }
    printf("catalog parity: %u mismatches over %zu spines + %zu toc + %zu manifest\n",
           mismatches, book.spineCount(), book.tocCount(), book.manifestCount());
    if (mismatches != 0) return 1;

    // Stored stylesheet loaded from this (fresh, from-disk) catalog open.
    const CssStylesheet* catSheet = catalog.stylesheet();
    printf("catalog stylesheet (from disk): %u rules, hash %08x\n",
           catSheet != nullptr ? catSheet->ruleCount : 0,
           catSheet != nullptr ? catSheet->contentHash : 0u);
    useCatalog = true;
  }

  // Book stylesheet from every CSS manifest item.
  Arena sheetArena(sheetBuf, sizeof(sheetBuf));
  CssStylesheetBuilder builder;
  builder.begin(sheetArena);
  uint32_t cssFiles = 0;
  for (size_t m = 0; m < book.manifestCount(); ++m) {
    const ManifestItem* item = book.manifestItem(m);
    if (strcmp(item->mediaType, "text/css") != 0) continue;
    const ZipEntry* entry = book.zip().find(item->href);
    if (entry == nullptr) continue;
    builder.addSheet(source, *entry, scratch);
    ++cssFiles;
  }
  static CssStylesheet sheet;
  sheet = builder.finish();
  printf("css: %u files, %u rules kept, %u sheets skipped (too large)\n", cssFiles,
         sheet.ruleCount, builder.skippedSheets());

  Hyphenator hyphenator;
  if (hyphPath != nullptr) {
    FILE* f = fopen(hyphPath, "rb");
    if (f != nullptr) {
      const size_t n = fread(hyphBuf, 1, sizeof(hyphBuf), f);
      fclose(f);
      hyphenator.init(hyphBuf, static_cast<uint32_t>(n));
    }
  }

  CheckFont font;
  LayoutParams params;
  params.font = &font;
  params.stylesheet = &sheet;
  params.defaultAlign = TextAlign::Justify;
  params.language = book.metadata().language[0] != '\0' ? book.metadata().language : "en";
  if (hyphenator.ready()) params.hyphenator = &hyphenator;

  uint32_t totalPages = 0;
  uint32_t okChapters = 0;
  uint32_t failChapters = 0;
  size_t maxHighWater = 0;
  static char catHref[512];
  for (size_t s = 0; s < book.spineCount(); ++s) {
    const char* href = nullptr;
    const ZipEntry* entry = nullptr;
    ZipEntry catEntry;
    if (useCatalog) {
      if (catalog.spineHref(s, catHref, sizeof(catHref)) == BookStatus::Ok &&
          catalog.spineEntry(s, &catEntry) == BookStatus::Ok) {
        href = catHref;
        entry = &catEntry;
      }
    } else {
      const ManifestItem* item = book.spineItem(s);
      href = item->href;
      entry = book.zip().find(item->href);
    }
    if (entry == nullptr) {
      printf("  spine[%zu] %s: MISSING\n", s, href != nullptr ? href : "?");
      ++failChapters;
      continue;
    }
    const ZipCatalog& zip = useCatalog ? catalog.zip() : book.zip();
    Arena layoutArena(scratchBuf, split ? layoutCap : sizeof(scratchBuf));
    Arena parseArena(parseBuf, parseCap);
    StatsSink sink;
    uint32_t pages = 0;
    const BookStatus ls = ChapterLayout::layout(source, zip, *entry, href, params, layoutArena, sink,
                                                &pages, nullptr, split ? &parseArena : nullptr);
    if (layoutArena.highWater() > maxHighWater) maxHighWater = layoutArena.highWater();
    totalPages += pages;
    if (ls == BookStatus::Ok) {
      ++okChapters;
      printf("  spine[%2zu] %-52.52s Ok  %4u pages  sizes:", s, href, pages);
      for (uint32_t i = 0; i < sink.sizeCount; ++i) printf(" %u", sink.sizes[i]);
      if (split) {
        printf("  (layout %zu, parse %zu B)", layoutArena.highWater(), parseArena.highWater());
      }
      printf("\n");
    } else {
      ++failChapters;
      printf("  spine[%2zu] %-52.52s %s", s, href, bookStatusName(ls));
      if (split) {
        printf("  (layout %zu/%zu refused %zu, parse %zu/%zu refused %zu B)", layoutArena.highWater(), layoutCap,
               layoutArena.failedAllocSize(), parseArena.highWater(), parseCap, parseArena.failedAllocSize());
      }
      printf("\n");
    }
  }
  printf("chapters: %u ok, %u failed; %u pages total; layout high water %zu B; "
         "book arena %zu B\n",
         okChapters, failChapters, totalPages, maxHighWater, bookArena.highWater());
  return failChapters == 0 ? 0 : 1;
}
