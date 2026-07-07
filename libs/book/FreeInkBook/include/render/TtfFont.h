#pragma once

// FreeInk SDK — TTF/OTF font engine for FreeInkBook (Phase 6).
//
// TtfFont implements BookFont over stb_truetype: real advances, kerning
// ('kern' and GPOS pairs), and on-demand glyph rasterization to 8-bit
// alpha bitmaps. The font file bytes are borrowed — point them at PSRAM, a
// memory-mapped region, or an arena-loaded SD file — and all cache memory
// comes from a caller-sized glyph arena, so the engine's no-heap rule holds.
//
// Caching: advances live in a direct-mapped table (collisions overwrite —
// always correct, occasionally recomputed). Rasterized glyphs append to the
// glyph arena through a direct-mapped table; when the arena fills, the whole
// cache flushes and rebuilds (bounded by construction, no LRU bookkeeping).
//
// Style flags are accepted but do not synthesize bold/italic — register the
// real face per style and route through FontChain. FontChain also gives
// per-codepoint fallback (user font → Latin default → CJK default) so mixed
// scripts render without tofu.
//
// Scope note: stb_truetype needs the whole font file addressable in memory.
// On ESP32-class devices that means fonts up to the free-PSRAM budget
// (typical Latin fonts are well under 1 MB). Streaming table access for
// multi-megabyte CJK fonts is the documented FreeType upgrade path in
// docs/freeink-book-design.md; the BookFont interface is identical either
// way.

#include <stdint.h>

#include "BookArena.h"
#include "BookFont.h"

namespace freeink {
namespace book {

class TtfFont : public RenderFont {
 public:
  // `data` is borrowed and must outlive the font. The glyph arena backs all
  // caches; 32–64 KB is comfortable for one active reading size.
  bool init(const uint8_t* data, uint32_t len, Arena& glyphArena);
  bool ready() const { return ready_; }

  bool hasGlyph(uint32_t codepoint) const override;

  // BookFont metrics.
  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override;
  int16_t lineHeight(uint16_t sizePx) override;
  int16_t ascent(uint16_t sizePx) override;
  int16_t kerning(uint32_t left, uint32_t right, uint16_t sizePx,
                  uint8_t styleFlags) override;

  // Rasterizes (and caches) one glyph. Returns nullptr for missing glyphs or
  // when a single glyph exceeds the whole arena. The bitmap stays valid
  // until the cache flushes — consume or blit before rasterizing many more.
  const GlyphBitmap* rasterize(uint32_t codepoint, uint16_t sizePx) override;

 private:
  struct AdvanceSlot {
    uint32_t key = 0xFFFFFFFF;  // codepoint
    int32_t glyphIndex = 0;
    int32_t unscaledAdvance = 0;
  };
  struct GlyphSlot {
    uint64_t key = UINT64_MAX;  // (codepoint << 16) | sizePx
    GlyphBitmap glyph{};
  };

  int32_t glyphIndexFor(uint32_t codepoint);
  void flushGlyphs();
  float scaleFor(uint16_t sizePx) const;

  // stb font handle storage (opaque here to keep stb out of public headers).
  alignas(8) uint8_t fontInfo_[176];
  bool ready_ = false;
  int32_t unitsAscent_ = 0;
  int32_t unitsDescent_ = 0;
  int32_t unitsLineGap_ = 0;

  Arena* glyphArena_ = nullptr;
  size_t glyphBase_ = 0;  // arena mark below which the tables live
  AdvanceSlot* advances_ = nullptr;
  GlyphSlot* glyphs_ = nullptr;

  static constexpr uint32_t kAdvanceSlots = 512;
  static constexpr uint32_t kGlyphSlots = 128;
};

// Per-codepoint fallback across up to four fonts. Metrics come from the
// first font that has the glyph; line height and ascent come from the
// primary so mixed-script lines share a stable baseline grid. Kerning
// applies only when both codepoints resolve to the same font.
class FontChain : public BookFont {
 public:
  bool add(RenderFont* font);

  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override;
  int16_t lineHeight(uint16_t sizePx) override;
  int16_t ascent(uint16_t sizePx) override;
  int16_t kerning(uint32_t left, uint32_t right, uint16_t sizePx,
                  uint8_t styleFlags) override;

  // The font that will draw `codepoint` (primary when nobody has it — its
  // .notdef box is the honest signal of a missing glyph).
  RenderFont* fontFor(uint32_t codepoint);

 private:
  RenderFont* fonts_[4] = {nullptr, nullptr, nullptr, nullptr};
  uint8_t count_ = 0;
};

}  // namespace book
}  // namespace freeink
