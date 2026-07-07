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
#include <render/TtfFont.h>

#include "FreeInkUIDisplayTarget.h"
#include "FreeInkUIFont.h"

namespace freeink {
namespace ui {

class BitmapBookFont : public book::RenderFont {
 public:
  explicit BitmapBookFont(const BitmapFont& font = kNotoSansFont) : font_(font) {}

  bool hasGlyph(uint32_t codepoint) const override { return glyphFor(codepoint) != nullptr; }

  int16_t advance(uint32_t codepoint, uint16_t, uint8_t) override {
    const FontGlyph* g = glyphFor(codepoint);
    if (g != nullptr) return g->xAdvance;
    const FontGlyph* space = glyphFor(' ');  // modest gap for the truly missing
    return space != nullptr ? space->xAdvance : font_.maxWidth;
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
  // Books use typographic punctuation (’ “ ” – — …) that compact bitmap
  // ranges rarely cover; degrade to the ASCII equivalent instead of a gap.
  static uint32_t normalized(uint32_t cp) {
    switch (cp) {
      case 0x2018: case 0x2019: case 0x02BC: return 0x27;  // '
      case 0x201C: case 0x201D: return 0x22;               // "
      case 0x2013: case 0x2014: case 0x2212: return 0x2D;  // -
      case 0x2026: return 0x2E;                            // . (of ...)
      case 0x00A0: return 0x20;                            // nbsp
      default: return cp;
    }
  }
  const FontGlyph* glyphFor(uint32_t cp) const {
    uint32_t codepoint = cp;
    if (codepoint < font_.first || codepoint > font_.last) codepoint = normalized(codepoint);
    if (codepoint < font_.first || codepoint > font_.last) return nullptr;
    return &font_.glyphs[codepoint - font_.first];
  }

  const BitmapFont& font_;
  book::GlyphBitmap glyph_{};
  uint8_t coverage_[64 * 64];  // one glyph, reused per rasterize() call
};

}  // namespace ui
}  // namespace freeink

namespace freeink {
namespace ui {

// DisplayTarget glyph fallback backed by a FreeInkBook TtfFont: UI chrome
// renders scripts the bitmap font can't pre-bake (Hangul, CJK, Cyrillic…)
// from a TTF on the card, sized per slot. Register with
// target.setGlyphFallback(&source).
class TtfGlyphSource : public RuntimeGlyphSource {
 public:
  void setFont(book::TtfFont* font) { font_ = font; }

  bool glyph(uint32_t codepoint, uint16_t pixelSize, Glyph* out) override {
    if (font_ == nullptr || !font_->hasGlyph(codepoint)) return false;
    const book::GlyphBitmap* g = font_->rasterize(codepoint, pixelSize);
    if (g == nullptr) return false;
    out->coverage = g->pixels;
    out->width = g->width;
    out->height = g->height;
    out->xoff = g->xoff;
    out->yoff = g->yoff;
    out->advance = g->advance;
    return true;
  }

  int16_t advance(uint32_t codepoint, uint16_t pixelSize) override {
    if (font_ == nullptr || !font_->hasGlyph(codepoint)) return 0;
    return font_->advance(codepoint, pixelSize, 0);
  }

 private:
  book::TtfFont* font_ = nullptr;
};

}  // namespace ui
}  // namespace freeink
