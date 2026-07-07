#pragma once

// FreeInk SDK — bundled-bitmap-font adapter for FreeInkBook (opt-in).
//
// BitmapBookFont exposes a FreeInkUI BitmapFont (the bundled Noto Sans, or
// anything gen_font.py produces) as a FreeInkBook RenderFont, so a reader
// renders books with NO font files on the card — drop it at the end of a
// FontChain as the always-present fallback. Like FreeInkUIGfxRenderer.h,
// this header is opt-in: it is only compilable in firmwares that also link
// FreeInkBook (it includes FreeInkBook's BookFont.h).
//
// Bitmap fonts have one baked size: metrics ignore the requested sizePx, so
// headings render at body size under this fallback. 1bpp glyphs convert to
// 0/255 coverage; 4bpp alpha fonts (gen_font.py --alpha) keep their real
// edge coverage, so anti-aliasing survives the adaptation.

#include <BookFont.h>

#include "FreeInkUIFont.h"

namespace freeink {
namespace ui {

class BitmapBookFont : public book::RenderFont {
 public:
  explicit BitmapBookFont(const BitmapFont& font = kNotoSansFont) : font_(font) {}

  bool hasGlyph(uint32_t codepoint) const override {
    return codepoint >= font_.first && codepoint <= font_.last;
  }

  int16_t advance(uint32_t codepoint, uint16_t, uint8_t) override {
    const FontGlyph* g = glyphFor(codepoint);
    return g != nullptr ? g->xAdvance : font_.maxWidth;
  }
  int16_t lineHeight(uint16_t) override { return font_.yAdvance; }
  int16_t ascent(uint16_t) override { return font_.ascent; }

  const book::GlyphBitmap* rasterize(uint32_t codepoint, uint16_t) override {
    const FontGlyph* g = glyphFor(codepoint);
    if (g == nullptr) return nullptr;
    const uint32_t pixels = static_cast<uint32_t>(g->width) * g->height;
    if (pixels > sizeof(coverage_)) return nullptr;
    const uint8_t* src = font_.bitmap + g->bitmapOffset;
    for (uint32_t i = 0; i < pixels; ++i) {
      if (font_.bpp == 4) {
        const uint8_t nibble = (i & 1) ? (src[i >> 1] & 0x0F) : (src[i >> 1] >> 4);
        coverage_[i] = static_cast<uint8_t>(nibble * 17);  // 0..15 → 0..255
      } else {
        coverage_[i] = (src[i >> 3] & (0x80u >> (i & 7))) ? 255 : 0;
      }
    }
    glyph_.pixels = coverage_;
    glyph_.width = g->width;
    glyph_.height = g->height;
    glyph_.xoff = g->xOffset;
    glyph_.yoff = g->yOffset;
    glyph_.advance = g->xAdvance;
    return &glyph_;
  }

 private:
  const FontGlyph* glyphFor(uint32_t codepoint) const {
    if (!hasGlyph(codepoint)) return nullptr;
    return &font_.glyphs[codepoint - font_.first];
  }

  const BitmapFont& font_;
  book::GlyphBitmap glyph_{};
  uint8_t coverage_[64 * 64];  // one glyph, reused per rasterize() call
};

}  // namespace ui
}  // namespace freeink
