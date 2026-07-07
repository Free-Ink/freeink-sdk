// Host-side tests for FreeInkBook Phase 6 (TTF font engine): real metrics
// from a real face (DejaVu Sans), kerning, glyph rasterization with the
// arena-bounded cache (including the flush-and-rebuild path), fallback
// chains, and an end-to-end layout driven by real font metrics.

#include <FreeInkBook.h>
#include <layout/ChapterLayout.h>
#include <render/PageRenderer.h>
#include <render/TtfFont.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

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

uint8_t fontData[1024 * 1024];
uint32_t fontDataLen = 0;
uint8_t glyphBuf[64 * 1024];
uint8_t bookBuf[64 * 1024];
uint8_t scratchBuf[256 * 1024];

void testMetrics(TtfFont& font) {
  CHECK(font.ready());
  CHECK(font.hasGlyph('A'));
  CHECK(font.hasGlyph(0x00E9));      // é
  CHECK(font.hasGlyph(0x0416));      // Ж — DejaVu covers Cyrillic
  CHECK(!font.hasGlyph(0x732B));     // 猫 — no CJK in DejaVu Sans

  const int16_t ascent = font.ascent(32);
  const int16_t lineHeight = font.lineHeight(32);
  CHECK(ascent > 16 && ascent < 40);
  CHECK(lineHeight >= 32 && lineHeight < 48);

  // Proportional metrics, stable across repeated (cached) lookups.
  const int16_t narrow = font.advance('i', 32, StyleNone);
  const int16_t wide = font.advance('W', 32, StyleNone);
  CHECK(narrow > 0);
  CHECK(wide > narrow * 2);
  CHECK_EQ(font.advance('i', 32, StyleNone), narrow);

  // Sizes scale roughly linearly.
  const int16_t at16 = font.advance('n', 16, StyleNone);
  const int16_t at32 = font.advance('n', 32, StyleNone);
  CHECK(at32 >= at16 * 2 - 1 && at32 <= at16 * 2 + 1);

  // DejaVu kerns AV/AV-style pairs (negative or zero, never positive here).
  const int16_t kernAV = font.kerning('A', 'V', 32, StyleNone);
  std::printf("  kerning A-V at 32px: %d\n", kernAV);
  CHECK(kernAV <= 0);
}

void testRasterization(TtfFont& font) {
  const GlyphBitmap* a = font.rasterize('A', 32);
  CHECK(a != nullptr);
  CHECK(a->width > 8 && a->width < 40);
  CHECK(a->height > 15 && a->height < 40);
  CHECK(a->advance > 0);
  CHECK(a->yoff < 0);  // glyph sits above the baseline
  int dark = 0;
  for (uint32_t i = 0; i < static_cast<uint32_t>(a->width) * a->height; ++i) {
    if (a->pixels[i] > 128) ++dark;
  }
  CHECK(dark > 20);

  // Cache hit returns the same slot.
  CHECK(font.rasterize('A', 32) == a);

  // Space has no bitmap but must not crash or return null metrics.
  const GlyphBitmap* space = font.rasterize(' ', 32);
  CHECK(space != nullptr);
  CHECK_EQ(space->width, 0);
  CHECK(space->advance > 0);

  // Hammer the cache far past the arena budget: 300 distinct (glyph, size)
  // pairs at a large size forces flush-and-rebuild generations; every
  // return must stay valid.
  int ok = 0;
  int expected = 0;
  for (uint32_t cp = 0x21; cp < 0x21 + 150; ++cp) {
    if (!font.hasGlyph(cp)) continue;  // C1 controls etc. — legitimately absent
    for (uint16_t size : {uint16_t{40}, uint16_t{44}}) {
      ++expected;
      const GlyphBitmap* g = font.rasterize(cp, size);
      if (g != nullptr && (g->width == 0 || g->pixels != nullptr)) ++ok;
    }
  }
  CHECK(expected > 200);
  CHECK_EQ(ok, expected);
  // And the original glyph still rasterizes after all that churn.
  const GlyphBitmap* again = font.rasterize('A', 32);
  CHECK(again != nullptr);
  CHECK_EQ(again->width, a->width);
}

void testFontChain(TtfFont& font, TtfFont& second) {
  FontChain chain;
  CHECK(chain.add(&font));
  CHECK(chain.add(&second));

  CHECK(chain.fontFor('A') == &font);          // primary has it
  CHECK(chain.fontFor(0x732B) == &font);       // nobody has it → primary notdef
  CHECK_EQ(chain.advance('A', 32, StyleNone), font.advance('A', 32, StyleNone));
  CHECK_EQ(chain.lineHeight(32), font.lineHeight(32));
  CHECK_EQ(chain.kerning('A', 'V', 32, StyleNone), font.kerning('A', 'V', 32, StyleNone));
}

void testLayoutWithRealFont(const char* fixturesDir, TtfFont& font) {
  char path[1024];
  std::snprintf(path, sizeof(path), "%s/minimal.epub", fixturesDir);
  HostFileSource source;
  CHECK(source.open(path));
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  LayoutParams params;
  params.font = &font;
  params.defaultAlign = TextAlign::Justify;

  struct GeoSink : PageSink {
    explicit GeoSink(TtfFont& f) : font(f) {}
    bool onPage(const Page& page) override {
      ++pages;
      for (uint16_t r = 0; r < page.runCount; ++r) {
        const PageTextRun& run = page.runs[r];
        int32_t width = 0;
        uint32_t i = 0;
        uint32_t prev = 0;
        while (i < run.len) {
          const uint8_t b = static_cast<uint8_t>(run.text[i]);
          uint32_t cp = b;
          uint32_t extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
          if (extra == 1) cp = b & 0x1F;
          if (extra == 2) cp = b & 0x0F;
          if (extra == 3) cp = b & 0x07;
          for (uint32_t k = 0; k < extra; ++k) {
            cp = (cp << 6) | (static_cast<uint8_t>(run.text[i + 1 + k]) & 0x3F);
          }
          i += 1 + extra;
          width += font.advance(cp, run.sizePx, run.styleFlags);
          if (prev != 0) width += font.kerning(prev, cp, run.sizePx, run.styleFlags);
          prev = cp;
        }
        if (run.x < 24 || run.x + width > 800 - 24) ++violations;
      }
      return true;
    }
    TtfFont& font;
    uint32_t pages = 0;
    int violations = 0;
  } sink(font);

  const ZipEntry* entry = book.zip().find(book.spineItem(0)->href);
  CHECK(entry != nullptr);
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(source, book.zip(), *entry, entry->name,
                                                  params, scratch, sink, nullptr)),
           static_cast<int>(BookStatus::Ok));
  CHECK(sink.pages >= 1);
  CHECK_EQ(sink.violations, 0);
}

void testPageRenderer(const char* fixturesDir, TtfFont& font) {
  char path[1024];
  std::snprintf(path, sizeof(path), "%s/minimal.epub", fixturesDir);
  HostFileSource source;
  CHECK(source.open(path));
  Arena bookArena(bookBuf, sizeof(bookBuf));
  Arena scratch(scratchBuf, sizeof(scratchBuf));
  Book book;
  CHECK_EQ(static_cast<int>(book.open(source, bookArena, scratch)),
           static_cast<int>(BookStatus::Ok));

  FontChain chain;
  CHECK(chain.add(&font));
  LayoutParams params;
  params.font = &chain;
  params.defaultAlign = TextAlign::Justify;

  static uint8_t monoFb[100 * 480];
  static uint8_t grayFb[800 * 480];
  struct FirstPageSink : PageSink {
    bool onPage(const Page& page) override {
      // Runs point into layout scratch — render inside the callback.
      FrameTarget mono{monoFb, 800, 480, 100, FrameFormat::Mono1Dithered};
      FrameTarget gray{grayFb, 800, 480, 800, FrameFormat::Gray8};
      std::memset(monoFb, 0xFF, sizeof(monoFb));
      std::memset(grayFb, 0xFF, sizeof(grayFb));
      PageRenderer::renderText(page, *fonts, mono);
      PageRenderer::renderText(page, *fonts, gray);
      rendered = true;
      return false;  // first page only
    }
    FontChain* fonts = nullptr;
    bool rendered = false;
  } sink;
  sink.fonts = &chain;

  const ZipEntry* entry = book.zip().find(book.spineItem(0)->href);
  CHECK(entry != nullptr);
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(source, book.zip(), *entry, entry->name,
                                                  params, scratch, sink, nullptr)),
           static_cast<int>(BookStatus::Ok));
  CHECK(sink.rendered);

  // Mono: ink landed inside the content box, margins stayed white.
  uint32_t blackBits = 0;
  for (uint32_t i = 0; i < sizeof(monoFb); ++i) {
    blackBits += static_cast<uint32_t>(__builtin_popcount(0xFF ^ monoFb[i]));
  }
  CHECK(blackBits > 2000);  // a page of text is a lot of ink
  int marginViolations = 0;
  for (int16_t y = 0; y < 480; ++y) {
    if (monoFb[y * 100] != 0xFF) ++marginViolations;  // left margin stays white
  }
  CHECK_EQ(marginViolations, 0);

  // Gray8: anti-aliasing is real — the page contains black, white, AND a
  // meaningful band of intermediate coverage values at glyph edges.
  uint32_t black = 0;
  uint32_t mid = 0;
  for (uint32_t i = 0; i < sizeof(grayFb); ++i) {
    if (grayFb[i] < 32) ++black;
    else if (grayFb[i] >= 64 && grayFb[i] < 224) ++mid;
  }
  CHECK(black > 1000);
  CHECK(mid > 500);  // edge pixels with partial coverage = anti-aliasing
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: %s <fixtures-build-dir> <font.ttf>\n", argv[0]);
    return 2;
  }

  {
    FILE* f = std::fopen(argv[2], "rb");
    CHECK(f != nullptr);
    if (f != nullptr) {
      fontDataLen = static_cast<uint32_t>(std::fread(fontData, 1, sizeof(fontData), f));
      std::fclose(f);
    }
    CHECK(fontDataLen > 100 * 1024);
  }

  static Arena glyphArena(glyphBuf, sizeof(glyphBuf));
  static TtfFont font;
  CHECK(font.init(fontData, fontDataLen, glyphArena));

  static uint8_t glyphBuf2[16 * 1024];
  static Arena glyphArena2(glyphBuf2, sizeof(glyphBuf2));
  static TtfFont second;
  CHECK(second.init(fontData, fontDataLen, glyphArena2));

  testMetrics(font);
  testRasterization(font);
  testFontChain(font, second);
  testLayoutWithRealFont(argv[1], font);
  testPageRenderer(argv[1], font);

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
