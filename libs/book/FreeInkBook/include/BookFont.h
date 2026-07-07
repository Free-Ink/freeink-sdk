#pragma once

// FreeInk SDK — font metrics interface for FreeInkBook layout.
//
// Layout needs only metrics: how wide is a codepoint at a size, how tall is a
// line. Rasterization stays behind the application's renderer (FreeInkUI
// DrawTarget, FreeType, or a test fake), so layout runs host-side with no
// font files at all. Sizes are pixels; style flags travel with each text run
// so bold/italic variants can report different advances.

#include <stdint.h>

namespace freeink {
namespace book {

// Per-run style bits, kept small enough to live in every page text run.
enum StyleFlags : uint8_t {
  StyleNone = 0,
  StyleBold = 1u << 0,
  StyleItalic = 1u << 1,
};

struct GlyphBitmap {
  const uint8_t* pixels;  // w*h, 8-bit coverage (0 = transparent)
  uint16_t width;
  uint16_t height;
  int16_t xoff;           // left bearing from pen position
  int16_t yoff;           // top offset from baseline (negative = above)
  int16_t advance;
};

class BookFont {
 public:
  virtual ~BookFont() = default;

  // Horizontal advance of one codepoint, in pixels.
  virtual int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) = 0;

  // Baseline-to-baseline line height for a size, in pixels.
  virtual int16_t lineHeight(uint16_t sizePx) = 0;

  // Distance from line top to baseline, in pixels.
  virtual int16_t ascent(uint16_t sizePx) = 0;

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
};

// A BookFont that can also rasterize — what PageRenderer and FontChain
// consume. TtfFont implements it over stb_truetype; FreeInkUI's opt-in
// FreeInkUIBookFont.h implements it over the bundled bitmap font so books
// render with no font files at all.
class RenderFont : public BookFont {
 public:
  virtual bool hasGlyph(uint32_t codepoint) const = 0;
  // Returns nullptr for missing glyphs. The bitmap must stay valid until the
  // next rasterize() call on the same font.
  virtual const GlyphBitmap* rasterize(uint32_t codepoint, uint16_t sizePx) = 0;
};

}  // namespace book
}  // namespace freeink
