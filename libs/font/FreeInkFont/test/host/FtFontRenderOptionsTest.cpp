// Host smoke test for FtFont::RenderOptions. Run via test/host/run.sh, which
// links this against the real vendored FreeType (all three
// FREEINK_FONT_ENABLE_* modules on) under ASan/UBSan against a real font.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "FtFont.h"

using freeink::font::FtFont;

namespace {

std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (fread(data.data(), 1, data.size(), f) != data.size()) {
    fprintf(stderr, "short read on %s\n", path);
    exit(1);
  }
  fclose(f);
  return data;
}

int checks = 0;
int failures = 0;
void expect(bool cond, const char* what) {
  ++checks;
  if (!cond) {
    ++failures;
    fprintf(stderr, "FAIL: %s\n", what);
  } else {
    printf("ok: %s\n", what);
  }
}

struct AllocationStats {
  size_t allocations = 0;
};

void* testAllocate(void* context, size_t size) {
  ++static_cast<AllocationStats*>(context)->allocations;
  return malloc(size);
}
void testDeallocate(void*, void* block) { free(block); }
void* testReallocate(void* context, void* block, size_t, size_t newSize) {
  ++static_cast<AllocationStats*>(context)->allocations;
  return realloc(block, newSize);
}

struct MemorySource {
  const std::vector<uint8_t>* bytes;
};

unsigned long readMemory(void* context, unsigned long offset, unsigned char* buffer, unsigned long count) {
  const auto& bytes = *static_cast<MemorySource*>(context)->bytes;
  if (offset > bytes.size() || count > bytes.size() - offset) return 0;
  if (count) std::memcpy(buffer, bytes.data() + offset, count);
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s font.ttf\n", argv[0]);
    return 1;
  }
  const std::vector<uint8_t> bytes = readFile(argv[1]);

  AllocationStats allocationStats;
  FtFont::MemoryCallbacks memory{&allocationStats, testAllocate, testDeallocate, testReallocate};
  expect(FtFont::configureMemory(&memory), "custom allocator can be configured before first use");

  FtFont::FaceInfo memoryInfo;
  char memoryFamily[128];
  expect(FtFont::inspectMemory(bytes.data(), static_cast<uint32_t>(bytes.size()), memoryInfo, memoryFamily,
                               sizeof(memoryFamily)) == FtFont::InspectResult::Ok,
         "inspectMemory() reads reusable face metadata");
  MemorySource source{&bytes};
  FtFont::FaceInfo streamInfo;
  char streamFamily[128];
  expect(FtFont::inspectStream(readMemory, &source, bytes.size(), streamInfo, streamFamily, sizeof(streamFamily)) ==
             FtFont::InspectResult::Ok,
         "inspectStream() reads metadata without retaining the face");
  expect(memoryFamily[0] && std::strcmp(memoryFamily, streamFamily) == 0 && memoryInfo.weight == streamInfo.weight &&
             memoryInfo.italic == streamInfo.italic,
         "memory and stream inspection report the same family and style");
  expect(allocationStats.allocations > 0, "FreeType routes allocations through the configured callbacks");
  expect(!FtFont::configureMemory(nullptr), "allocator configuration freezes after first SDK use");

  FtFont font;
  expect(font.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16, 400, false), "init() loads the face");
  expect(font.hasGlyph('A'), "face covers 'A'");

  constexpr uint32_t kFractionalSize26_6 = 17 * 64 + 32;
  const FtFont::GlyphId glyphA = font.glyphId('A');
  expect(glyphA != 0, "glyphId() exposes the cmap result");
  FtFont::GlyphMetrics fractionalMetrics;
  expect(font.metricsGlyph26_6(glyphA, kFractionalSize26_6, fractionalMetrics) && fractionalMetrics.advance26_6 > 0,
         "metricsGlyph26_6() preserves fractional pixel sizing");
  expect(font.rasterizeGlyph26_6(glyphA, kFractionalSize26_6) != nullptr,
         "rasterizeGlyph26_6() renders an explicit glyph ID");
  FtFont::LineMetrics fractionalLine;
  expect(font.lineMetrics26_6(kFractionalSize26_6, fractionalLine) && fractionalLine.height26_6 > 0,
         "lineMetrics26_6() reports fixed-point line metrics");
  expect(font.kerningGlyphs26_6(glyphA, font.glyphId('V'), kFractionalSize26_6) < 0,
         "kerningGlyphs26_6() returns fractional glyph-ID kerning");

  const auto* bmDefault = font.rasterize('A', 16);
  expect(bmDefault != nullptr, "rasterize('A') under HintingMode::Default returns a bitmap");
  expect(bmDefault && bmDefault->width > 0 && bmDefault->height > 0, "Default bitmap has nonzero dimensions");
  const uint16_t defaultWidth = bmDefault->width;
  const uint16_t defaultHeight = bmDefault->height;
  const int16_t defaultAdvance = font.advance('A', 16, 0);

  FtFont::RenderOptions none;
  none.hinting = FtFont::HintingMode::None;
  font.setRenderOptions(none);
  const auto* bmNone = font.rasterize('A', 16);
  expect(bmNone != nullptr, "rasterize('A') under HintingMode::None returns a bitmap");
  expect(bmNone && bmNone->width > 0 && bmNone->height > 0, "None bitmap has nonzero dimensions");
  // Default and None are DELIBERATELY not required to match: Default keeps
  // FreeType's always-on phantom-point advance rounding (so it matches this
  // library's original pre-RenderOptions behavior in every build config —
  // see loadFlagsFor()'s comment), while None is an explicit opt-in for
  // disabling that too. The actual "opt-in has no side effect" guarantee is
  // checked across build configurations, not within one binary — see
  // FtFontDefaultParityHash.cpp / run.sh, which hash Default's output in a
  // full-flags build and a minimal build and diff them.
  (void)defaultWidth;
  (void)defaultHeight;
  (void)defaultAdvance;

  FtFont::RenderOptions autoHint;
  autoHint.hinting = FtFont::HintingMode::Auto;
  autoHint.stemDarkening = true;
  font.setRenderOptions(autoHint);
  expect(font.rasterize('A', 16) != nullptr,
         "rasterize('A') under HintingMode::Auto (registered autofit module) returns a bitmap");

  autoHint.embolden26_6 = 32;
  autoHint.slant16_16 = 9209;
  expect(font.setRenderOptions(autoHint) && font.rasterize26_6('A', kFractionalSize26_6) != nullptr,
         "generic embolden and slant options compose with fixed-point rendering");

  FtFont::RenderOptions light;
  light.hinting = FtFont::HintingMode::Light;
  font.setRenderOptions(light);
  expect(font.rasterize('A', 16) != nullptr, "rasterize('A') under HintingMode::Light returns a bitmap");

  FtFont::RenderOptions native35;
  native35.hinting = FtFont::HintingMode::Native;
  native35.interpreterVersion = 35;
  font.setRenderOptions(native35);
  expect(font.rasterize('A', 16) != nullptr, "rasterize('A') under HintingMode::Native v35 returns a bitmap");

  FtFont::RenderOptions native40;
  native40.hinting = FtFont::HintingMode::Native;
  native40.interpreterVersion = 40;
  font.setRenderOptions(native40);
  expect(font.rasterize('A', 16) != nullptr, "rasterize('A') under HintingMode::Native v40 returns a bitmap");

  FtFont::RenderOptions mono;
  mono.monochrome = true;
  font.setRenderOptions(mono);
  const auto* bmMono = font.rasterize('A', 16);
  expect(bmMono != nullptr, "rasterize('A') under monochrome (registered raster1 module) returns a bitmap");
  if (bmMono) {
    // Font.h's GlyphBitmap contract is 8-bit coverage at stride `width`, NOT
    // FreeType's native 1-bpp packed mono format — reading every contract
    // byte here is exactly what a real consumer (e.g. FreeInkBook's
    // PageRenderer) does, so this must run clean under ASan or the contract
    // is violated (it previously was: this read past the packed buffer).
    bool sawSetPixel = false;
    bool allValidCoverage = true;
    for (uint32_t y = 0; y < bmMono->height; ++y) {
      for (uint32_t x = 0; x < bmMono->width; ++x) {
        const uint8_t coverage = bmMono->pixels[size_t(y) * bmMono->width + x];
        if (coverage != 0x00 && coverage != 0xFF) allValidCoverage = false;
        if (coverage == 0xFF) sawSetPixel = true;
      }
    }
    expect(allValidCoverage, "monochrome coverage bytes are all 0x00 or 0xFF (true 8-bit expansion, not packed bits)");
    expect(sawSetPixel, "monochrome 'A' has at least one set pixel");
  }

  // Metrics still answer sanely after all the option churn above.
  expect(font.advance('A', 16, 0) > 0, "advance('A') is positive after option changes");
  font.kerning('A', 'V', 16, 0);  // must not crash; not every font carries a kern table
  expect(true, "kerning('A','V') does not crash");

  // Cross-face isolation: interpreterVersion/stemDarkening are FreeType
  // library-wide properties, so each face must restore its own effective
  // properties immediately before loading a glyph. Use a fixture glyph whose
  // native v35 and v40 outputs differ; otherwise this regression test would
  // be unable to distinguish a leaked property from a correct restore.
  {
    constexpr uint32_t kProbe = 'g';
    constexpr uint16_t kProbeSize = 16;
    FtFont defaultFace;
    expect(defaultFace.init(bytes.data(), static_cast<uint32_t>(bytes.size()), kProbeSize, 400, false),
           "default face init() succeeds");
    FtFont::RenderOptions defaultOptions;
    expect(defaultFace.setRenderOptions(defaultOptions), "Default render options are supported");
    const auto* bmDefault1 = defaultFace.rasterize(kProbe, kProbeSize);
    expect(bmDefault1 != nullptr, "Default face renders the probe glyph");
    const uint16_t defaultWidth = bmDefault1 ? bmDefault1->width : 0;
    const uint16_t defaultHeight = bmDefault1 ? bmDefault1->height : 0;
    std::vector<uint8_t> defaultPixels;
    if (bmDefault1 && defaultWidth && defaultHeight)
      defaultPixels.assign(bmDefault1->pixels, bmDefault1->pixels + size_t(defaultWidth) * defaultHeight);

    FtFont nativeFace;
    expect(nativeFace.init(bytes.data(), static_cast<uint32_t>(bytes.size()), kProbeSize, 400, false),
           "native face init() succeeds");
    FtFont::RenderOptions nativeOptions;
    nativeOptions.hinting = FtFont::HintingMode::Native;
    nativeOptions.interpreterVersion = 35;
    expect(nativeFace.setRenderOptions(nativeOptions), "Native v35 render options are supported");
    const auto* bmNative = nativeFace.rasterize(kProbe, kProbeSize);
    expect(bmNative != nullptr, "Native v35 face renders the probe glyph");
    std::vector<uint8_t> nativeProbePixels;
    if (bmNative && bmNative->width && bmNative->height)
      nativeProbePixels.assign(bmNative->pixels, bmNative->pixels + size_t(bmNative->width) * bmNative->height);
    const bool nativeDiffers = bmNative && (bmNative->width != defaultWidth || bmNative->height != defaultHeight ||
                                            nativeProbePixels != defaultPixels);
    expect(nativeDiffers, "fixture probe distinguishes Native v35 from Default/v40");
    if (!nativeDiffers) {
      fprintf(stderr,
              "test setup error: fixture glyph/size does not "
              "distinguish Native v35 from Default/v40\n");
      return 1;
    }

    // Rendering Native v35 on another face changes the library-global
    // interpreter setting. The Default face must restore v40 before its next
    // load and reproduce its original output exactly.
    const auto* bmDefault2 = defaultFace.rasterize(kProbe, kProbeSize);
    expect(bmDefault2 != nullptr, "Default face renders again after Native v35");
    bool defaultRestored = bmDefault2 && bmDefault2->width == defaultWidth && bmDefault2->height == defaultHeight;
    if (defaultRestored) {
      for (uint32_t i = 0; i < uint32_t(defaultWidth) * defaultHeight; ++i) {
        if (bmDefault2->pixels[i] != defaultPixels[i]) defaultRestored = false;
      }
    }
    expect(defaultRestored, "Default face restores v40 after another face rendered Native v35");

    // Retain the reverse-direction proof: Native v35 also restores itself
    // after the Default face has rendered.
    const auto* bmNative1 = nativeFace.rasterize(kProbe, kProbeSize);
    expect(bmNative1 != nullptr, "Native v35 face renders again after Default");
    const uint16_t nativeWidth = bmNative1 ? bmNative1->width : 0;
    const uint16_t nativeHeight = bmNative1 ? bmNative1->height : 0;
    std::vector<uint8_t> nativePixels;
    if (bmNative1 && nativeWidth && nativeHeight)
      nativePixels.assign(bmNative1->pixels, bmNative1->pixels + size_t(nativeWidth) * nativeHeight);
    const auto* bmDefault3 = defaultFace.rasterize(kProbe, kProbeSize);
    (void)bmDefault3;
    const auto* bmNative2 = nativeFace.rasterize(kProbe, kProbeSize);
    expect(bmNative2 != nullptr, "Native v35 face renders after Default rendered again");
    bool nativeRestored = bmNative2 && bmNative2->width == nativeWidth && bmNative2->height == nativeHeight;
    if (nativeRestored) {
      for (uint32_t i = 0; i < uint32_t(nativeWidth) * nativeHeight; ++i) {
        if (bmNative2->pixels[i] != nativePixels[i]) nativeRestored = false;
      }
    }
    expect(nativeRestored, "Native v35 face restores its interpreter after Default rendered");
  }

  printf("\n%d/%d checks passed\n", checks - failures, checks);
  return failures == 0 ? 0 : 1;
}
