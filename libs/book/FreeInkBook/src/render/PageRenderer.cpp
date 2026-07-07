// FreeInkBook — Page → 1bpp framebuffer compositor.

#include "render/PageRenderer.h"

#include <string.h>

#include "render/ImageRenderer.h"

namespace freeink {
namespace book {

namespace {

constexpr uint8_t kInkThreshold = 120;  // Mono1Sharp: coverage above this is ink

// Bayer 4×4 (0..255 domain) — the same matrix DisplayTarget uses for alpha
// fonts, so SDK chrome and book pages dither identically.
constexpr uint8_t kBayer4[4][4] = {
    {15, 135, 45, 165}, {195, 75, 225, 105}, {60, 180, 30, 150}, {240, 120, 210, 90}};

// Maps a page-logical pixel into panel space (transforms match DisplayTarget).
// Returns false when the pixel is out of bounds.
inline bool toPanel(const FrameTarget& t, int32_t x, int32_t y, int32_t& px, int32_t& py) {
  const int32_t lw = t.rotation == FrameRotation::Portrait ||
                             t.rotation == FrameRotation::PortraitInverted
                         ? t.height
                         : t.width;
  const int32_t lh = lw == t.height ? t.width : t.height;
  if (x < 0 || y < 0 || x >= lw || y >= lh) return false;
  switch (t.rotation) {
    case FrameRotation::Portrait:  // 90° CW
      px = y;
      py = t.height - 1 - x;
      break;
    case FrameRotation::PortraitInverted:  // 90° CCW
      px = t.width - 1 - y;
      py = x;
      break;
    case FrameRotation::UpsideDown:  // 180°
      px = t.width - 1 - x;
      py = t.height - 1 - y;
      break;
    case FrameRotation::None:
      px = x;
      py = y;
      break;
  }
  return true;
}

inline void setBlack(const FrameTarget& t, int32_t x, int32_t y) {
  int32_t px, py;
  if (!toPanel(t, x, y, px, py)) return;
  t.framebuffer[py * t.widthBytes + (px >> 3)] &=
      static_cast<uint8_t>(~(0x80u >> (px & 7)));
}

// Applies one glyph coverage sample (0 = transparent, 255 = full ink).
inline void inkPixel(const FrameTarget& t, int32_t x, int32_t y, uint8_t coverage) {
  if (coverage == 0) return;
  switch (t.format) {
    case FrameFormat::Gray8: {
      int32_t px, py;
      if (!toPanel(t, x, y, px, py)) return;
      // Ink darkens: keep the darker of existing and new (overlaps compose).
      uint8_t* dst = &t.framebuffer[py * t.widthBytes + px];
      const uint8_t lum = static_cast<uint8_t>(255 - coverage);
      if (lum < *dst) *dst = lum;
      break;
    }
    case FrameFormat::Mono1Sharp:
      if (coverage >= kInkThreshold) setBlack(t, x, y);
      break;
    case FrameFormat::Mono1Dithered:
      if (coverage >= kBayer4[y & 3][x & 3]) setBlack(t, x, y);
      break;
  }
}

uint32_t decodeUtf8(const char* text, uint32_t len, uint32_t& i) {
  const uint8_t b0 = static_cast<uint8_t>(text[i]);
  uint32_t cp = b0;
  uint32_t extra = 0;
  if (b0 >= 0xF0) {
    cp = b0 & 0x07;
    extra = 3;
  } else if (b0 >= 0xE0) {
    cp = b0 & 0x0F;
    extra = 2;
  } else if (b0 >= 0xC0) {
    cp = b0 & 0x1F;
    extra = 1;
  }
  ++i;
  while (extra > 0 && i < len && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) {
    cp = (cp << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
    ++i;
    --extra;
  }
  return cp;
}

struct ImageBlit {
  const FrameTarget* target;
  int16_t x;
  int16_t y;
  uint8_t rowBits[128];  // (panel width / 8) headroom

  static bool onRow(void* user, uint16_t rowY, const uint8_t* gray, uint16_t width) {
    ImageBlit* self = static_cast<ImageBlit*>(user);
    const FrameTarget& t = *self->target;
    if (t.format == FrameFormat::Gray8) {
      for (uint16_t i = 0; i < width; ++i) {
        int32_t px, py;
        if (toPanel(t, self->x + i, self->y + rowY, px, py)) {
          t.framebuffer[py * t.widthBytes + px] = gray[i];
        }
      }
      return true;
    }
    if (width > sizeof(self->rowBits) * 8) width = sizeof(self->rowBits) * 8;
    ditherRowOrdered(gray, width, rowY, self->rowBits);
    for (uint16_t px = 0; px < width; ++px) {
      if (self->rowBits[px >> 3] & (0x80u >> (px & 7))) {
        setBlack(t, self->x + px, self->y + rowY);
      }
    }
    return true;
  }
};

}  // namespace

void PageRenderer::renderText(const Page& page, FontChain& fonts, const FrameTarget& target) {
  for (uint16_t r = 0; r < page.runCount; ++r) {
    const PageTextRun& run = page.runs[r];
    int32_t penX = run.x;
    uint32_t i = 0;
    uint32_t prev = 0;
    while (i < run.len) {
      const uint32_t cp = decodeUtf8(run.text, run.len, i);
      if (prev != 0) penX += fonts.kerning(prev, cp, run.sizePx, run.styleFlags);
      RenderFont* font = fonts.fontFor(cp);
      const GlyphBitmap* glyph = font != nullptr ? font->rasterize(cp, run.sizePx) : nullptr;
      if (glyph != nullptr) {
        for (uint16_t gy = 0; gy < glyph->height; ++gy) {
          const uint8_t* srcRow = glyph->pixels + static_cast<uint32_t>(gy) * glyph->width;
          const int32_t dy = run.baselineY + glyph->yoff + gy;
          for (uint16_t gx = 0; gx < glyph->width; ++gx) {
            inkPixel(target, penX + glyph->xoff + gx, dy, srcRow[gx]);
          }
        }
      }
      penX += fonts.advance(cp, run.sizePx, run.styleFlags);
      prev = cp;
    }
  }
}

BookStatus PageRenderer::renderImages(const Page& page, BookSource& source,
                                      const ZipCatalog& zip, Arena& scratch,
                                      const FrameTarget& target) {
  BookStatus worst = BookStatus::Ok;
  for (uint16_t m = 0; m < page.imageCount; ++m) {
    const PageImage& image = page.images[m];
    ImageBlit blit{&target, image.x, image.y, {}};
    const BookStatus status =
        ImageRenderer::render(source, zip, image, scratch, ImageBlit::onRow, &blit);
    // Unsupported formats leave their reserved space blank; real I/O errors
    // are reported (with the rest of the page still drawn).
    if (status != BookStatus::Ok && status != BookStatus::Unsupported) worst = status;
  }
  return worst;
}

BookStatus PageRenderer::render(const Page& page, FontChain& fonts, BookSource& source,
                                const ZipCatalog& zip, Arena& scratch,
                                const FrameTarget& target) {
  renderText(page, fonts, target);
  return renderImages(page, source, zip, scratch, target);
}

}  // namespace book
}  // namespace freeink
