#pragma once

// Optional adapter: FreeInkUI DrawTarget backed by the GfxRenderer used in
// CrossPoint and CrossInk (the same class in both forks). Header-only and
// include-driven like FreeInkUIInputManager.h — FreeInkUI itself stays
// dependency-free, and this header only compiles in firmwares that provide
// <GfxRenderer.h>.
//
// Components draw through this class so the firmware's fonts, bidi,
// truncation, dithering, and framebuffer management stay centralized in
// GfxRenderer. FreeInkUI font ids are small slots (FONT_SMALL/BODY/TITLE);
// setFont() binds each slot to a real GfxRenderer font id.

#include <FreeInkUI.h>

// Only compile the adapter when the firmware actually provides GfxRenderer.
// Without this guard, merely including this header (directly or transitively)
// in an SDK app that has no GfxRenderer hard-fails the build. Apps that don't
// use CrossPoint should draw through FreeInkUIDisplayTarget.h instead.
#if __has_include(<GfxRenderer.h>)
#define FREEINK_HAVE_GFX_RENDERER 1
#include <GfxRenderer.h>

#include <algorithm>
#include <string>
#include <vector>

namespace freeink {
namespace ui {

class GfxRendererTarget final : public DrawTarget {
 public:
  static constexpr FontId FONT_SMALL = 0;
  static constexpr FontId FONT_BODY = 1;
  static constexpr FontId FONT_TITLE = 2;
  static constexpr size_t FONT_SLOTS = 3;

  explicit GfxRendererTarget(const GfxRenderer& renderer) : renderer(renderer) {
    for (size_t i = 0; i < FONT_SLOTS; ++i) fonts[i] = 0;
  }

  void setFont(const FontId slot, const int gfxFontId) {
    if (slot < FONT_SLOTS) fonts[slot] = gfxFontId;
  }

  DeviceContext deviceContext() const {
    DeviceContext device;
    device.width = static_cast<int16_t>(renderer.getScreenWidth());
    device.height = static_cast<int16_t>(renderer.getScreenHeight());
    device.orientation = static_cast<Orientation>(renderer.getOrientation());
    // Same inverse-of-render-rotation selector as DisplayTarget: this makes
    // touchToLogical() agree case-for-case with GfxRenderer::tapToLogical, so
    // FreeInkUI components and the firmware's own tap path map taps identically.
    device.touchOrientation = touchOrientationFor(device.orientation);
    device.hasButtons = true;
    return device;
  }

  Size measureText(const FontId font, const char* text, const TextStyle style) const override {
    if (!text) return {};
    const int fontId = gfxFont(font);
    return Size{static_cast<int16_t>(renderer.getTextWidth(fontId, text, fontStyle(style))),
                static_cast<int16_t>(renderer.getLineHeight(fontId))};
  }

  int16_t lineHeight(const FontId font) const override {
    return static_cast<int16_t>(renderer.getLineHeight(gfxFont(font)));
  }

  void fill(const Rect rect, const Paint paint, const uint8_t radius = 0,
            const uint8_t corners = CornersAll) override {
    if (rect.empty()) return;
    switch (paint.kind) {
      case PaintKind::Solid:
      case PaintKind::Dither:
        if (paint.color == Color::Transparent) break;
        if (radius > 0) {
          // fillRoundedRect dithers through the same Color levels.
          if (corners == CornersAll) {
            renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, radius, gfxColor(paint.color));
          } else {
            renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, radius, (corners & CornerTopLeft) != 0,
                                     (corners & CornerTopRight) != 0, (corners & CornerBottomLeft) != 0,
                                     (corners & CornerBottomRight) != 0, gfxColor(paint.color));
          }
        } else if (paint.kind == PaintKind::Solid && (paint.color == Color::Black || paint.color == Color::White)) {
          renderer.fillRect(rect.x, rect.y, rect.width, rect.height, paint.color == Color::Black);
        } else {
          renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, gfxColor(paint.color));
        }
        break;
      case PaintKind::Bitmap:
        bitmap(rect, paint.bitmap.bitmap, paint.bitmap.mode, Paint::solid(Color::Black));
        break;
      case PaintKind::None:
      default:
        break;
    }
  }

  void stroke(const Rect rect, const Paint paint, const uint8_t width, const uint8_t radius = 0,
              const uint8_t corners = CornersAll) override {
    if (rect.empty() || width == 0 || paint.kind == PaintKind::None) return;
    const bool black = paint.color != Color::White;
    if (radius > 0) {
      if (corners == CornersAll) {
        renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, width, radius, black);
      } else {
        renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, width, radius,
                                 (corners & CornerTopLeft) != 0, (corners & CornerTopRight) != 0,
                                 (corners & CornerBottomLeft) != 0, (corners & CornerBottomRight) != 0, black);
      }
    } else {
      renderer.drawRect(rect.x, rect.y, rect.width, rect.height, width, black);
    }
  }

  void line(const Point from, const Point to, const uint8_t width, const Paint paint) override {
    if (paint.kind == PaintKind::None) return;
    renderer.drawLine(from.x, from.y, to.x, to.y, width, paint.color != Color::White);
  }

  void triangle(const Point a, const Point b, const Point c, const Paint paint) override {
    if (paint.kind == PaintKind::None) return;
    const int xs[3] = {a.x, b.x, c.x};
    const int ys[3] = {a.y, b.y, c.y};
    renderer.fillPolygon(xs, ys, 3, paint.color != Color::White);
  }

  void text(const Rect rect, const char* text, const TextStyle style) override {
    if (!text || rect.empty()) return;
    const int fontId = gfxFont(style.font);
    const EpdFontFamily::Style epdStyle = fontStyle(style);
    const bool black = !style.inverted && style.color != Color::White;
    const int lh = renderer.getLineHeight(fontId);
    const uint8_t maxLines = style.maxLines > 0 ? style.maxLines : 1;

    if (style.rotation != Rotation::None) {
      // Single-line rotated text. This renderer natively supports CW90 (text
      // flows top-to-bottom along the rect's left edge); other rotations draw
      // unrotated, horizontally centered, as a visible-but-degraded fallback.
      if (style.rotation == Rotation::CW90) {
        // drawTextRotated90CW renders upward from the given y (the y is the
        // bottom end of the run), so alignment offsets place that end.
        const std::string textLine = renderer.truncatedText(fontId, text, rect.height, epdStyle);
        const int textLen = renderer.getTextWidth(fontId, textLine.c_str(), epdStyle);
        int y = rect.y + textLen;  // Left = run starts at the rect top
        if (style.align == TextAlign::Center) y = rect.y + (rect.height + textLen) / 2;
        if (style.align == TextAlign::Right) y = rect.bottom();
        const int x = rect.x + std::max(0, (rect.width - lh) / 2);
        renderer.drawTextRotated90CW(fontId, x, y, textLine.c_str(), black, epdStyle);
        return;
      }
    }

    const auto drawAligned = [&](const std::string& textLine, const int y) {
      int x = rect.x;
      if (style.align != TextAlign::Left) {
        const int textW = renderer.getTextWidth(fontId, textLine.c_str(), epdStyle);
        x = style.align == TextAlign::Center ? rect.x + (rect.width - textW) / 2 : rect.x + rect.width - textW;
        if (x < rect.x) x = rect.x;
      }
      renderer.drawText(fontId, x, y, textLine.c_str(), black, epdStyle);
    };

    if (maxLines == 1) {
      const std::string textLine = renderer.truncatedText(fontId, text, rect.width, epdStyle);
      drawAligned(textLine, rect.y + std::max(0, (rect.height - lh) / 2));
      return;
    }

    const std::vector<std::string> lines = renderer.wrappedText(fontId, text, rect.width, maxLines, epdStyle);
    const int blockH = static_cast<int>(lines.size()) * lh;
    int y = rect.y + std::max(0, (rect.height - blockH) / 2);
    for (const auto& textLine : lines) {
      drawAligned(textLine, y);
      y += lh;
    }
  }

  void bitmap(const Rect rect, const BitmapRef bitmap, const BitmapMode mode,
              const Paint foreground = Paint::solid(Color::Black),
              const Rotation rotation = Rotation::None) override {
    if (!bitmap || rect.empty()) return;

    // Mask1: square 1-bit masks in drawIcon's rotated layout (this renderer's
    // icon asset convention). Drawn at native size; Tile modes repeat.
    // Per-element rotation is unsupported for this layout — ship rotated
    // asset variants, or use BW1 masks which rotate below.
    if (bitmap.format == BitmapFormat::Mask1) {
      if (mode == BitmapMode::Tile || mode == BitmapMode::TileX || mode == BitmapMode::TileY) {
        const bool tileX = mode != BitmapMode::TileY;
        const bool tileY = mode != BitmapMode::TileX;
        for (int y = rect.y; y < rect.bottom(); y += bitmap.height) {
          for (int x = rect.x; x < rect.right(); x += bitmap.width) {
            renderer.drawIcon(bitmap.data, x, y, bitmap.width, bitmap.height);
            if (!tileX) break;
          }
          if (!tileY) break;
        }
      } else {
        renderer.drawIcon(bitmap.data, rect.x + (rect.width - bitmap.width) / 2,
                          rect.y + (rect.height - bitmap.height) / 2, bitmap.width, bitmap.height);
      }
      return;
    }

    // BW1: row-major, MSB-first, set bit = ink. The SDK's shared sampling
    // helper handles every BitmapMode (including Stretch/Contain/Cover
    // scaling and tiling); each ink pixel lands via drawPixel — adequate for
    // icons and pattern fills on a 1-bit panel.
    if (bitmap.format != BitmapFormat::BW1) return;
    const bool black = foreground.color != Color::White;
    forEachBitmapPixel(
        rect, bitmap, mode, [&](const int16_t px, const int16_t py) { renderer.drawPixel(px, py, black); },
        rotation);
  }

 private:
  const GfxRenderer& renderer;
  int fonts[FONT_SLOTS];

  int gfxFont(const FontId slot) const { return slot < FONT_SLOTS ? fonts[slot] : fonts[FONT_BODY]; }

  // FreeInkUI colors map onto GfxRenderer's Bayer dither levels.
  static ::Color gfxColor(const Color color) {
    switch (color) {
      case Color::White:
        return ::Color::White;
      case Color::LightGray:
        return ::Color::LightGray;
      case Color::DarkGray:
        return ::Color::DarkGray;
      case Color::Black:
        return ::Color::Black;
      case Color::Transparent:
      default:
        return ::Color::Clear;
    }
  }

  static EpdFontFamily::Style fontStyle(const TextStyle& style) {
    return style.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  }
};

template <size_t MaxInteractions = 1>
class GfxRendererFrame {
 public:
  explicit GfxRendererFrame(const GfxRenderer& renderer, const int smallFontId = 0, const int bodyFontId = 0,
                            const int titleFontId = 0)
      : target(renderer), frame(target, target.deviceContext(), input, interactions) {
    target.setFont(GfxRendererTarget::FONT_SMALL, smallFontId);
    target.setFont(GfxRendererTarget::FONT_BODY, bodyFontId);
    target.setFont(GfxRendererTarget::FONT_TITLE, titleFontId);
  }

  GfxRendererTarget target;
  InputSnapshot input{};
  InteractionBuffer<MaxInteractions> interactions;
  Frame<MaxInteractions> frame;
};

}  // namespace ui
}  // namespace freeink

#endif  // __has_include(<GfxRenderer.h>)
