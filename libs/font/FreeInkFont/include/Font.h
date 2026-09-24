#pragma once

// FreeInk SDK — standalone font metrics + rasterization interface (FreeInkFont).
//
// A font engine decoupled from any layout/book code: any consumer (a book
// renderer, a UI toolkit, or third-party firmware such as CrossPoint) can
// measure and rasterize real fonts without pulling in an EPUB engine.
// FreeInkBook re-exports these types under its historical freeink::book names
// (BookFont == Font, RenderFont == RasterFont) — see FreeInkBook/BookFont.h.
//
// `Font` is metrics-only (advance/lineHeight/ascent/kerning/ligature/covers) so
// a layout engine can run host-side with no font files. `RasterFont` adds
// on-demand glyph rasterization. Sizes are pixels; style flags travel with each
// text run so bold/italic variants can report different advances.

#include <stdint.h>

namespace freeink {
namespace font {

// Per-run style bits, kept small enough to live in every page text run.
enum StyleFlags : uint8_t {
  StyleNone = 0,
  StyleBold = 1u << 0,
  StyleItalic = 1u << 1,
  StyleUnderline = 1u << 2,
  StyleSuperscript = 1u << 3,
  StyleSubscript = 1u << 4,
};

struct GlyphBitmap {
  const uint8_t* pixels;  // w*h, 8-bit coverage (0 = transparent)
  uint16_t width;
  uint16_t height;
  int16_t xoff;           // left bearing from pen position
  int16_t yoff;           // top offset from baseline (negative = above)
  int16_t advance;
};

class Font {
 public:
  virtual ~Font() = default;

  // Horizontal advance of one codepoint, in pixels.
  virtual int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) = 0;

  // Baseline-to-baseline line height for a size, in pixels.
  virtual int16_t lineHeight(uint16_t sizePx) = 0;

  // Distance from line top to baseline, in pixels.
  virtual int16_t ascent(uint16_t sizePx) = 0;

  // Ligature substitution: the single codepoint that replaces the pair
  // (left, right) — e.g. 'f'+'i' → U+FB01 — or 0 when none applies. Pairs
  // chain (U+FB00+'i' → U+FB03). Layout applies this during measurement AND
  // bakes the substituted codepoint into page-run text, so rendering and
  // caching need no ligature logic of their own.
  virtual uint32_t ligature(uint32_t left, uint32_t right, uint8_t styleFlags) {
    (void)left;
    (void)right;
    (void)styleFlags;
    return 0;
  }

  // Kerning adjustment applied between `left` and `right`, in pixels
  // (usually zero or negative). Layout adds this during measurement so line
  // breaks account for it; fonts without kerning data keep the default.
  virtual int16_t kerning(uint32_t left, uint32_t right, uint16_t sizePx, uint8_t styleFlags) {
    (void)left;
    (void)right;
    (void)sizePx;
    (void)styleFlags;
    return 0;
  }

  // Whether the font can draw `codepoint` at all. Layout uses this to keep a
  // shaping substitution (Arabic presentation forms) only when the font has
  // the substituted glyph, falling back to the base letter otherwise. The
  // default claims everything so metrics-only fakes stay simple.
  virtual bool covers(uint32_t codepoint) {
    (void)codepoint;
    return true;
  }
};

// A Font that can also rasterize — what PageRenderer and FontChain
// consume. TtfFont implements it over stb_truetype; FreeInkUI's opt-in
// FreeInkUIBookFont.h implements it over the bundled bitmap font so books
// render with no font files at all.
class RasterFont : public Font {
 public:
  virtual bool hasGlyph(uint32_t codepoint) const = 0;
  bool covers(uint32_t codepoint) override { return hasGlyph(codepoint); }
  // Returns nullptr for missing glyphs. The bitmap must stay valid until the
  // next rasterize() call on the same font.
  virtual const GlyphBitmap* rasterize(uint32_t codepoint, uint16_t sizePx) = 0;
};

}  // namespace font
}  // namespace freeink
