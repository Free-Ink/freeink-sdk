#pragma once

// FreeInk SDK — page compositor for FreeInkBook.
//
// Draws a laid-out (or cache-read) Page into a raw 1-bit framebuffer using
// the same convention as FreeInkDisplay and FreeInkUI's DisplayTarget:
// MSB-first, row-major, SET bit = white, CLEAR bit = black ink. Text glyphs
// come from a FontChain (per-codepoint fallback, coverage thresholded to
// ink); images stream through ImageRenderer with ordered dithering. This is
// the render half of "layout once, render many": a page turn is one cache
// read plus this call — no parsing, no layout, no allocation beyond the
// caller's scratch arena for image decode.

#include <stdint.h>

#include "BookArena.h"
#include "BookStorage.h"
#include "BookTypes.h"
#include "epub/ZipCatalog.h"
#include "layout/ChapterLayout.h"
#include "render/TtfFont.h"

namespace freeink {
namespace book {

// Pixel formats. Glyphs carry real 8-bit coverage from the rasterizer; the
// format decides how that anti-aliasing reaches the panel:
//   Mono1Dithered — 1bpp (MSB-first, set = white); edge coverage is ordered-
//                   dithered, matching FreeInkUI's alpha-font rendering. The
//                   default for 1-bit panels (Sticky, Xteink).
//   Mono1Sharp    — 1bpp with a hard coverage threshold; crispest stems, no
//                   edge softening.
//   Gray8         — one byte per pixel (0 = black); full anti-aliasing for
//                   grayscale pipelines (IT8951's 16 levels, X3's 2-bit
//                   planes after quantization downstream).
enum class FrameFormat : uint8_t { Mono1Dithered, Mono1Sharp, Gray8 };

// Rotation from page-logical coordinates into the panel-native framebuffer.
// The transforms match FreeInkUI's DisplayTarget (Portrait = 90° CW), so a
// portrait reader lays out pages at (panelHeight × panelWidth) and text/UI
// agree on "up". width/height/widthBytes always describe the PANEL.
enum class FrameRotation : uint8_t { None, Portrait, PortraitInverted, UpsideDown };

struct FrameTarget {
  uint8_t* framebuffer;
  int16_t width;       // panel-native
  int16_t height;      // panel-native
  int16_t widthBytes;  // bytes per panel row: (width+7)/8 for Mono1*, width for Gray8
  FrameFormat format = FrameFormat::Mono1Dithered;
  FrameRotation rotation = FrameRotation::None;
};

class PageRenderer {
 public:
  // Draws the page's text runs. Glyph advances and kerning replay exactly
  // what layout measured (same FontChain), so runs land on their recorded
  // positions.
  static void renderText(const Page& page, FontChain& fonts, const FrameTarget& target);

  // Draws the page's images (streaming decode + Bayer dither). `source`/
  // `zip` are the open book; scratch is released before returning.
  static BookStatus renderImages(const Page& page, BookSource& source, const ZipCatalog& zip,
                                 Arena& scratch, const FrameTarget& target);

  // Text + images.
  static BookStatus render(const Page& page, FontChain& fonts, BookSource& source,
                           const ZipCatalog& zip, Arena& scratch, const FrameTarget& target);
};

}  // namespace book
}  // namespace freeink
