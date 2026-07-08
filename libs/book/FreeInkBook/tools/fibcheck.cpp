// fibcheck — runs a real EPUB through the whole FreeInkBook pipeline on the
// host and reports what happened: container parse, metadata, TOC, CSS rules,
// per-chapter layout status, page counts, font sizes seen (dropcaps and
// heading scales show up here), and arena high-water marks.
//
// Build/run via test/host/fibcheck.sh <book.epub> [hyph-en-us.fibh]

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
  for (int a = 1; a < argc; ++a) {
    if (strncmp(argv[a], "--layout=", 9) == 0) {
      layoutCap = static_cast<size_t>(strtoul(argv[a] + 9, nullptr, 10));
    } else if (strncmp(argv[a], "--parse=", 8) == 0) {
      parseCap = static_cast<size_t>(strtoul(argv[a] + 8, nullptr, 10));
    } else if (bookPath == nullptr) {
      bookPath = argv[a];
    } else {
      hyphPath = argv[a];
    }
  }
  if (bookPath == nullptr) {
    printf("usage: %s [--layout=BYTES --parse=BYTES] <book.epub> [hyph.fibh]\n", argv[0]);
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
  for (size_t s = 0; s < book.spineCount(); ++s) {
    const ManifestItem* item = book.spineItem(s);
    const ZipEntry* entry = book.zip().find(item->href);
    if (entry == nullptr) {
      printf("  spine[%zu] %s: MISSING\n", s, item->href);
      ++failChapters;
      continue;
    }
    Arena layoutArena(scratchBuf, split ? layoutCap : sizeof(scratchBuf));
    Arena parseArena(parseBuf, parseCap);
    StatsSink sink;
    uint32_t pages = 0;
    const BookStatus ls = ChapterLayout::layout(source, book.zip(), *entry, entry->name, params, layoutArena, sink,
                                                &pages, nullptr, split ? &parseArena : nullptr);
    if (layoutArena.highWater() > maxHighWater) maxHighWater = layoutArena.highWater();
    totalPages += pages;
    if (ls == BookStatus::Ok) {
      ++okChapters;
      printf("  spine[%2zu] %-52.52s Ok  %4u pages  sizes:", s, item->href, pages);
      for (uint32_t i = 0; i < sink.sizeCount; ++i) printf(" %u", sink.sizes[i]);
      if (split) {
        printf("  (layout %zu, parse %zu B)", layoutArena.highWater(), parseArena.highWater());
      }
      printf("\n");
    } else {
      ++failChapters;
      printf("  spine[%2zu] %-52.52s %s", s, item->href, bookStatusName(ls));
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
