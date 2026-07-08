// Host-side unit tests for FreeInkBook Phase 1 (container layer). The engine
// is freestanding C++, so the ZIP catalog, streaming inflate, and package/TOC
// parsing run here with no device in the loop — including the memory-ceiling
// assertions that keep "RAM is O(one page)" an enforced invariant rather
// than a hope. Run with test/host/run.sh.

#include <FreeInkBook.h>
#include <epub/PackageParsers.h>

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

// Phase 1 memory ceilings. The book arena holds everything an open book
// retains (ZIP catalog + package + TOC); scratch peaks at one streaming
// reader (decompressor + 32 KB window + buffers). Growing these numbers is a
// regression that needs a design reason, not a bump.
constexpr size_t kBookArenaCeiling = 24 * 1024;
constexpr size_t kScratchCeiling = 96 * 1024;

uint8_t bookBuf[64 * 1024];
uint8_t scratchBuf[256 * 1024];

char fixturePath[1024];
const char* fixturesDir = nullptr;

const char* fixture(const char* name) {
  std::snprintf(fixturePath, sizeof(fixturePath), "%s/%s", fixturesDir, name);
  return fixturePath;
}

// --- Arena ------------------------------------------------------------------

void testArena() {
  uint8_t buf[64];
  Arena arena(buf, sizeof(buf));

  void* a = arena.alloc(10, 1);
  CHECK(a == buf);
  void* b = arena.alloc(4, 4);
  CHECK(b == buf + 12);  // aligned up from 10
  CHECK_EQ(arena.used(), 16u);

  const size_t marked = arena.mark();
  CHECK(arena.alloc(16, 1) != nullptr);
  arena.release(marked);
  CHECK_EQ(arena.used(), 16u);
  CHECK_EQ(arena.highWater(), 32u);  // high water survives release

  CHECK_EQ(arena.failedAllocSize(), 0u);
  CHECK(arena.alloc(1024, 1) == nullptr);  // exhaustion returns nullptr
  CHECK_EQ(arena.used(), 16u);             // failed alloc does not move cursor
  CHECK_EQ(arena.failedAllocSize(), 1024u);
  CHECK(arena.alloc(100, 1) == nullptr);
  CHECK_EQ(arena.failedAllocSize(), 1024u);  // records the LARGEST refusal

  const char* s = arena.strdup("hello");
  CHECK_STREQ(s, "hello");

  arena.reset();
  CHECK_EQ(arena.used(), 0u);
  CHECK_EQ(arena.highWater(), 32u);
  CHECK_EQ(arena.failedAllocSize(), 1024u);  // survives reset; cleared by init

  arena.init(buf, sizeof(buf));
  CHECK_EQ(arena.failedAllocSize(), 0u);
}

// --- Path resolution ---------------------------------------------------------

void testResolveHref() {
  uint8_t buf[2048];
  Arena arena(buf, sizeof(buf));
  const char* frag = nullptr;

  CHECK_STREQ(resolveHref(arena, "OEBPS", "text/ch1.xhtml", &frag), "OEBPS/text/ch1.xhtml");
  CHECK(frag == nullptr);

  CHECK_STREQ(resolveHref(arena, "OEBPS", "./text/../text/ch2.xhtml", &frag),
              "OEBPS/text/ch2.xhtml");

  CHECK_STREQ(resolveHref(arena, "OEBPS/text", "../images/cover.jpg", &frag),
              "OEBPS/images/cover.jpg");

  CHECK_STREQ(resolveHref(arena, "", "ch1.xhtml", &frag), "ch1.xhtml");

  CHECK_STREQ(resolveHref(arena, "OEBPS", "text/ch%201.xhtml#s1", &frag),
              "OEBPS/text/ch 1.xhtml");
  CHECK_STREQ(frag, "s1");

  // ".." past the root clamps at the root instead of escaping the container.
  CHECK_STREQ(resolveHref(arena, "OEBPS", "../../../etc/passwd", &frag), "etc/passwd");

  char dir[64];
  CHECK(dirName("OEBPS/content.opf", dir, sizeof(dir)));
  CHECK_STREQ(dir, "OEBPS");
  CHECK(dirName("content.opf", dir, sizeof(dir)));
  CHECK_STREQ(dir, "");
}

// --- Full book open ----------------------------------------------------------

void testMinimalBook(const char* name) {
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  CHECK(source.open(fixture(name)));

  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  CHECK_STREQ(book.metadata().title, "The Test Book");
  CHECK_STREQ(book.metadata().author, "Ann Author");
  CHECK_STREQ(book.metadata().language, "en");
  CHECK_STREQ(book.metadata().identifier, "urn:uuid:freeinkbook-fixture-0001");

  CHECK_EQ(book.spineCount(), 8u);
  CHECK_STREQ(book.spineItem(0)->href, "OEBPS/text/ch 1.xhtml");
  CHECK_STREQ(book.spineItem(1)->href, "OEBPS/text/ch2.xhtml");
  CHECK_STREQ(book.spineItem(2)->href, "OEBPS/text/ch3.xhtml");
  CHECK_STREQ(book.spineItem(3)->href, "OEBPS/text/ch4.xhtml");
  CHECK_STREQ(book.spineItem(4)->href, "OEBPS/text/ch5.xhtml");
  CHECK_STREQ(book.spineItem(5)->href, "OEBPS/text/ch6.xhtml");
  CHECK_STREQ(book.spineItem(6)->href, "OEBPS/text/ch7.xhtml");
  CHECK_STREQ(book.spineItem(7)->href, "OEBPS/text/ch8.xhtml");
  CHECK(book.spineItem(8) == nullptr);

  // EPUB 2 <meta name="cover"> promotes its manifest item to cover-image.
  {
    bool found = false;
    for (size_t i = 0; i < book.manifestCount(); ++i) {
      const ManifestItem* it = book.manifestItem(i);
      if (it != nullptr && strcmp(it->id, "img1") == 0) {
        CHECK(it->isCoverImage);
        found = true;
      }
    }
    CHECK(found);
  }

  // TOC comes from the EPUB 3 nav document (preferred over the NCX).
  CHECK_EQ(book.tocCount(), 3u);
  CHECK_STREQ(book.tocEntry(0)->title, "Chapter One");  // text spans nested elements
  CHECK_STREQ(book.tocEntry(0)->href, "OEBPS/text/ch 1.xhtml");
  CHECK_EQ(book.tocEntry(0)->depth, 0);
  CHECK_STREQ(book.tocEntry(1)->title, "Section 1.1");
  CHECK_STREQ(book.tocEntry(1)->fragment, "s1");
  CHECK_EQ(book.tocEntry(1)->depth, 1);
  CHECK_STREQ(book.tocEntry(2)->title, "Chapter Two");
  CHECK_EQ(book.tocEntry(2)->depth, 0);

  // Stream chapter 1 in page-sized chunks; verify contents and exact length.
  const size_t marked = scratch.mark();
  ZipEntryReader reader;
  CHECK_EQ(static_cast<int>(book.openItem(*book.spineItem(0), &reader, scratch)),
           static_cast<int>(BookStatus::Ok));
  char content[8192];
  uint32_t total = 0;
  for (;;) {
    const int32_t n = reader.read(content + total, 1000);
    CHECK(n >= 0);
    if (n <= 0) break;
    total += static_cast<uint32_t>(n);
    CHECK(total < sizeof(content));
  }
  content[total] = '\0';
  CHECK_EQ(total, reader.entry()->uncompressedSize);
  CHECK(std::strstr(content, "Call me Ishmael") != nullptr);
  scratch.release(marked);

  // Chapter 2 is generated large (well past the 32 KB inflate window) —
  // stream it whole and verify both ends arrived intact.
  const size_t marked2 = scratch.mark();
  ZipEntryReader big;
  CHECK_EQ(static_cast<int>(book.openItem(*book.spineItem(1), &big, scratch)),
           static_cast<int>(BookStatus::Ok));
  CHECK(big.entry()->uncompressedSize > 100 * 1024);
  char head[65] = {0};
  char tail[65] = {0};
  uint32_t bigTotal = 0;
  char chunk[1237];  // odd size so reads never align with the inflate window
  for (;;) {
    const int32_t n = big.read(chunk, sizeof(chunk));
    CHECK(n >= 0);
    if (n <= 0) break;
    const uint32_t un = static_cast<uint32_t>(n);
    if (bigTotal < 64) {
      const uint32_t take = un < 64 - bigTotal ? un : 64 - bigTotal;
      memcpy(head + bigTotal, chunk, take);
    }
    if (un >= 64) {
      memcpy(tail, chunk + un - 64, 64);
    } else {
      memmove(tail, tail + un, 64 - un);
      memcpy(tail + 64 - un, chunk, un);
    }
    bigTotal += un;
  }
  CHECK_EQ(bigTotal, big.entry()->uncompressedSize);
  CHECK(std::strstr(head, "<?xml") != nullptr);
  CHECK(std::strstr(tail, "</html>") != nullptr);
  scratch.release(marked2);

  // Memory ceilings — the design's headline invariant.
  std::printf("  %-14s bookArena high water %zu B, scratch high water %zu B\n", name,
              bookArena.highWater(), scratch.highWater());
  CHECK(bookArena.highWater() < kBookArenaCeiling);
  CHECK(scratch.highWater() < kScratchCeiling);
  CHECK_EQ(scratch.used(), 0u);  // everything transient was released
}

void testNcxOnlyBook() {
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  CHECK(source.open(fixture("ncxonly.epub")));

  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  CHECK_STREQ(book.metadata().title, "Legacy Book");
  CHECK_STREQ(book.metadata().language, "de");
  CHECK_EQ(book.spineCount(), 1u);
  CHECK_STREQ(book.spineItem(0)->href, "OEBPS/ch1.xhtml");

  // No nav document — the TOC falls back to the EPUB 2 NCX.
  CHECK_EQ(book.tocCount(), 1u);
  CHECK_STREQ(book.tocEntry(0)->title, "Erstes Kapitel");
  CHECK_STREQ(book.tocEntry(0)->href, "OEBPS/ch1.xhtml");
  CHECK_EQ(book.tocEntry(0)->depth, 0);
}

void testNotAnEpub() {
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));

  HostFileSource source;
  CHECK(source.open(fixture("garbage.bin")));
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::NotZip));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <fixtures-build-dir>\n", argv[0]);
    return 2;
  }
  fixturesDir = argv[1];

  testArena();
  testResolveHref();
  testMinimalBook("minimal.epub");
  testMinimalBook("stored.epub");  // same book, stored (uncompressed) entries
  testNcxOnlyBook();
  testNotAnEpub();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
