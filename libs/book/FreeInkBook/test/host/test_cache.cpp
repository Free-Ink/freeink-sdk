// Host-side tests for FreeInkBook Phase 3 (page cache + anchors): cached
// pages must be equivalent to direct layout, stale generations must be
// rejected, reading position must survive a font-size change exactly, and
// the page-read path must stay tiny (no ZIP/inflate/XML anywhere near it).

#include <FreeInkBook.h>
#include <cache/PageCache.h>
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

using namespace freeink::book;

class HostFileSource : public BookSource {
 public:
  ~HostFileSource() override {
    if (file_ != nullptr) std::fclose(file_);
  }
  bool open(const char* path) {
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

// FILE*-backed cache storage rooted in the test build directory.
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
  int32_t readBackAt(uint32_t offset, void* dst, uint32_t len) override {
    if (writeFile_ == nullptr) return -1;
    std::fflush(writeFile_);
    FILE* f = std::fopen(writePath_, "rb");
    if (f == nullptr) return -1;
    std::fseek(f, static_cast<long>(offset), SEEK_SET);
    const size_t n = std::fread(dst, 1, len, f);
    std::fclose(f);
    return static_cast<int32_t>(n);
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

class FakeFont : public BookFont {
 public:
  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override {
    (void)codepoint;
    return static_cast<int16_t>(sizePx / 2 + ((styleFlags & StyleBold) ? 1 : 0));
  }
  int16_t lineHeight(uint16_t sizePx) override { return static_cast<int16_t>(sizePx + 4); }
  int16_t ascent(uint16_t sizePx) override { return static_cast<int16_t>(sizePx); }
};

// Captures one specific page from a live layout for comparison with the
// cached copy.
class CapturePageSink : public PageSink {
 public:
  explicit CapturePageSink(uint32_t wanted) : wanted_(wanted) {}

  bool onPage(const Page& page) override {
    ++pages;
    if (page.pageIndex == wanted_) {
      runCount = page.runCount;
      charStart = page.charStart;
      textLen = 0;
      for (uint16_t r = 0; r < page.runCount && textLen + page.runs[r].len < sizeof(text); ++r) {
        std::memcpy(text + textLen, page.runs[r].text, page.runs[r].len);
        textLen += page.runs[r].len;
      }
      text[textLen] = '\0';
    }
    return true;
  }

  uint32_t wanted_;
  uint32_t pages = 0;
  uint16_t runCount = 0;
  uint32_t charStart = 0;
  char text[32 * 1024];
  uint32_t textLen = 0;
};

uint8_t bookBuf[64 * 1024];
uint8_t scratchBuf[256 * 1024];
uint8_t cacheBuf[128 * 1024];

char fixturePath[1024];
const char* fixturesDir = nullptr;

const char* fixture(const char* name) {
  std::snprintf(fixturePath, sizeof(fixturePath), "%s/%s", fixturesDir, name);
  return fixturePath;
}

LayoutParams makeParams(BookFont& font, uint16_t baseSizePx) {
  LayoutParams params;
  params.pageWidth = 800;
  params.pageHeight = 480;
  params.baseSizePx = baseSizePx;
  params.font = &font;
  return params;
}

// Lays out the big chapter into the cache; returns the generation hash.
uint32_t buildCache(HostFileSource& source, Book& book, HostCacheStorage& cache,
                    const LayoutParams& params, Arena& scratch, uint32_t* pagesOut) {
  const uint32_t hash = layoutGenerationHash(params, /*fontFingerprint=*/1);
  char name[64];
  CHECK(pageCacheName(1, hash, name, sizeof(name)));

  const size_t marked = scratch.mark();
  PageCacheWriter writer;
  CHECK(writer.begin(cache, name, hash, scratch));
  const ZipEntry* entry = book.zip().find(book.spineItem(1)->href);
  CHECK(entry != nullptr);
  uint32_t pages = 0;
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(source, book.zip(), *entry, entry->name, params, scratch, writer,
                                                  &pages)),
           static_cast<int>(BookStatus::Ok));
  CHECK(writer.finish());
  CHECK_EQ(writer.pageCount(), pages);
  scratch.release(marked);
  if (pagesOut != nullptr) *pagesOut = pages;
  return hash;
}

void testRoundtripAndStaleness(HostCacheStorage& cache) {
  HostFileSource source;
  CHECK(source.open(fixture("minimal.epub")));
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  FakeFont font;
  const LayoutParams params = makeParams(font, 16);
  uint32_t pages = 0;
  const uint32_t hash = buildCache(source, book, cache, params, scratch, &pages);
  CHECK(pages > 10);

  char name[64];
  CHECK(pageCacheName(1, hash, name, sizeof(name)));

  // Reopen from cache and compare a middle page against direct layout.
  Arena cacheArena(cacheBuf, sizeof(cacheBuf));
  PageCacheReader reader;
  CHECK_EQ(static_cast<int>(reader.open(cache, name, hash, cacheArena)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(reader.pageCount(), pages);

  const uint32_t middle = pages / 2;
  CapturePageSink direct(middle);
  {
    const size_t marked = scratch.mark();
    const ZipEntry* entry = book.zip().find(book.spineItem(1)->href);
    CHECK_EQ(static_cast<int>(ChapterLayout::layout(source, book.zip(), *entry, entry->name, params, scratch, direct,
                                                    nullptr)),
             static_cast<int>(BookStatus::Ok));
    scratch.release(marked);
  }

  const size_t marked = scratch.mark();
  Page page{};
  CHECK_EQ(static_cast<int>(reader.readPage(middle, scratch, &page)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(page.runCount, direct.runCount);
  CHECK_EQ(page.charStart, direct.charStart);
  char cachedText[32 * 1024];
  uint32_t cachedLen = 0;
  for (uint16_t r = 0; r < page.runCount && cachedLen + page.runs[r].len < sizeof(cachedText);
       ++r) {
    std::memcpy(cachedText + cachedLen, page.runs[r].text, page.runs[r].len);
    cachedLen += page.runs[r].len;
  }
  cachedText[cachedLen] = '\0';
  CHECK_EQ(cachedLen, direct.textLen);
  CHECK(std::strcmp(cachedText, direct.text) == 0);

  // The page-read memory story: index open + one page decode, nothing else.
  std::printf("  cache open %zu B, page read %zu B\n", cacheArena.highWater(),
              scratch.used() - marked);
  CHECK(cacheArena.highWater() < 8 * 1024);
  CHECK(scratch.used() - marked < 16 * 1024);
  scratch.release(marked);

  // Anchors are monotonically nondecreasing and start at 0.
  CHECK_EQ(reader.charStart(0), 0u);
  for (uint32_t p = 1; p < reader.pageCount(); ++p) {
    CHECK(reader.charStart(p) >= reader.charStart(p - 1));
  }

  // A different generation hash must refuse the file.
  PageCacheReader stale;
  Arena staleArena(cacheBuf, sizeof(cacheBuf));
  CHECK_EQ(static_cast<int>(stale.open(cache, name, hash ^ 0xDEAD, staleArena)),
           static_cast<int>(BookStatus::Stale));

  // A torn file (footer cut off) must also be refused.
  const int64_t size = cache.fileSize(name);
  CHECK(size > 0);
  uint8_t* copy = new uint8_t[static_cast<size_t>(size) - 5];
  CHECK_EQ(cache.readAt(name, 0, copy, static_cast<uint32_t>(size) - 5),
           static_cast<int32_t>(size - 5));
  CHECK(cache.beginWrite("torn.fibp"));
  CHECK(cache.write(copy, static_cast<uint32_t>(size) - 5));
  CHECK(cache.endWrite());
  delete[] copy;
  PageCacheReader torn;
  Arena tornArena(cacheBuf, sizeof(cacheBuf));
  const BookStatus tornStatus = torn.open(cache, "torn.fibp", hash, tornArena);
  CHECK(tornStatus == BookStatus::Stale || tornStatus == BookStatus::NotFound);
}

// The position-restore promise: pick a spot on a page at 16 px, relayout the
// chapter at 22 px into a second generation, and land on the page containing
// the same character — exactly, not approximately.
void testPositionMigration(HostCacheStorage& cache) {
  HostFileSource source;
  CHECK(source.open(fixture("minimal.epub")));
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  FakeFont font;
  const LayoutParams small = makeParams(font, 16);
  const LayoutParams large = makeParams(font, 22);
  uint32_t smallPages = 0;
  uint32_t largePages = 0;
  const uint32_t smallHash = buildCache(source, book, cache, small, scratch, &smallPages);
  const uint32_t largeHash = buildCache(source, book, cache, large, scratch, &largePages);
  CHECK(smallHash != largeHash);
  CHECK(largePages > smallPages);  // bigger text, more pages

  char smallName[64];
  char largeName[64];
  CHECK(pageCacheName(1, smallHash, smallName, sizeof(smallName)));
  CHECK(pageCacheName(1, largeHash, largeName, sizeof(largeName)));

  Arena smallArena(cacheBuf, sizeof(cacheBuf) / 2);
  Arena largeArena(cacheBuf + sizeof(cacheBuf) / 2, sizeof(cacheBuf) / 2);
  PageCacheReader smallReader;
  PageCacheReader largeReader;
  CHECK_EQ(static_cast<int>(smallReader.open(cache, smallName, smallHash, smallArena)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(static_cast<int>(largeReader.open(cache, largeName, largeHash, largeArena)),
           static_cast<int>(BookStatus::Ok));

  // "Reading position" = first character of small-generation page 7.
  const uint32_t position = smallReader.charStart(7);
  const uint32_t largePage = largeReader.pageForChar(position);
  CHECK(largeReader.charStart(largePage) <= position);
  if (largePage + 1 < largeReader.pageCount()) {
    CHECK(largeReader.charStart(largePage + 1) > position);
  }

  // And back: the large page's own anchor maps into small-generation page 7
  // or its immediate neighborhood (the same text, re-flowed).
  const uint32_t back = smallReader.pageForChar(largeReader.charStart(largePage));
  CHECK(back <= 7 && back + 1 >= 7);
}

}  // namespace


// Incremental build: pages must be readable from the WRITER while the
// chapter is still laying out (readBackAt), a suspended build must commit a
// partial file the reader accepts (isPartial, watermark page count, build
// progress fields), and the partial's pages must be byte-identical to the
// same pages of a finished build.
void testPartialCacheAndMidBuildRead(HostCacheStorage& cache) {
  HostFileSource source;
  CHECK(source.open(fixture("minimal.epub")));
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  FakeFont font;
  const LayoutParams params = makeParams(font, 16);
  const uint32_t hash = layoutGenerationHash(params, /*fontFingerprint=*/1);
  char name[64];
  CHECK(pageCacheName(7, hash, name, sizeof(name)));
  const ZipEntry* entry = book.zip().find(book.spineItem(1)->href);
  CHECK(entry != nullptr);

  // Reference: capture page 2 from a plain one-shot layout.
  CapturePageSink direct(2);
  {
    const size_t marked = scratch.mark();
    CHECK_EQ(static_cast<int>(ChapterLayout::layout(source, book.zip(), *entry, entry->name,
                                                    params, scratch, direct, nullptr)),
             static_cast<int>(BookStatus::Ok));
    scratch.release(marked);
  }
  CHECK(direct.pages > 10);

  // Incremental: writer as sink, stepped session, stop mid-chapter.
  const size_t marked = scratch.mark();
  PageCacheWriter writer;
  CHECK(writer.begin(cache, name, hash, scratch));
  ChapterLayoutSession session;
  CHECK_EQ(static_cast<int>(session.begin(source, &book.zip(), source, *entry, entry->name,
                                          params, scratch, writer)),
           static_cast<int>(BookStatus::Ok));
  while (!session.done() && session.pagesEmitted() < 6) {
    CHECK_EQ(static_cast<int>(session.step(2)), static_cast<int>(BookStatus::Ok));
  }
  CHECK(!session.done());  // genuinely suspended mid-chapter
  const uint32_t watermark = writer.pageCount();
  CHECK(watermark >= 6u);
  CHECK(watermark < direct.pages);

  // Mid-build read-back of page 2 matches the live layout byte for byte.
  {
    Arena pageArena(cacheBuf, sizeof(cacheBuf));
    Page page{};
    CHECK_EQ(static_cast<int>(writer.readPage(2, pageArena, &page)),
             static_cast<int>(BookStatus::Ok));
    CHECK_EQ(page.runCount, direct.runCount);
    CHECK_EQ(page.charStart, direct.charStart);
    char text[32 * 1024];
    uint32_t textLen = 0;
    for (uint16_t r = 0; r < page.runCount && textLen + page.runs[r].len < sizeof(text); ++r) {
      std::memcpy(text + textLen, page.runs[r].text, page.runs[r].len);
      textLen += page.runs[r].len;
    }
    text[textLen] = '\0';
    CHECK(std::strcmp(text, direct.text) == 0);
    // Index navigation against the writer works mid-build.
    CHECK_EQ(writer.pageForChar(writer.charStart(3)), 3u);
  }

  // Suspend: commits a partial the reader accepts and serves.
  writer.setTotalChars(12345);  // chars-so-far watermark (any value works here)
  CHECK(writer.suspend(1000, 4000));
  session.abort();
  scratch.release(marked);

  Arena cacheArena(cacheBuf, sizeof(cacheBuf));
  PageCacheReader reader;
  CHECK_EQ(static_cast<int>(reader.open(cache, name, hash, cacheArena)),
           static_cast<int>(BookStatus::Ok));
  CHECK(reader.isPartial());
  CHECK_EQ(reader.pageCount(), watermark);
  CHECK_EQ(reader.buildBytesConsumed(), 1000u);
  CHECK_EQ(reader.buildBytesTotal(), 4000u);
  {
    Page page{};
    CHECK_EQ(static_cast<int>(reader.readPage(2, cacheArena, &page)),
             static_cast<int>(BookStatus::Ok));
    CHECK_EQ(page.runCount, direct.runCount);
    CHECK_EQ(page.charStart, direct.charStart);
  }
  // A partial under a different generation is still Stale.
  {
    Arena tmpArena(cacheBuf, sizeof(cacheBuf));
    PageCacheReader stale;
    CHECK_EQ(static_cast<int>(stale.open(cache, name, hash + 1, tmpArena)),
             static_cast<int>(BookStatus::Stale));
  }
  cache.remove(name);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: %s <fixtures-build-dir> <cache-dir>\n", argv[0]);
    return 2;
  }
  fixturesDir = argv[1];
  HostCacheStorage cache(argv[2]);

  testRoundtripAndStaleness(cache);
  testPositionMigration(cache);
  testPartialCacheAndMidBuildRead(cache);

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
