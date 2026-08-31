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
#include <type_traits>
#include <utility>
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
    // The panel's bezel-covered edge pixels (BoardConfig viewableInsets,
    // rotated into this orientation). Screen seeds its content rect from
    // safeRect(), so everything laid out through it clears the glass;
    // deliberate full-bleed paint keeps using screen().
    {
      int top = 0, right = 0, bottom = 0, left = 0;
      renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
      device.safeArea = Insets{static_cast<int16_t>(top), static_cast<int16_t>(right),
                               static_cast<int16_t>(bottom), static_cast<int16_t>(left)};
    }
    // Same inverse-of-render-rotation selector as DisplayTarget: this makes
    // touchToLogical() agree case-for-case with GfxRenderer::tapToLogical, so
    // FreeInkUI components and the firmware's own tap path map taps identically.
    device.touchOrientation = touchOrientationFor(device.orientation);
    device.hasButtons = true;
    // Board viewable insets (bezel / rounded-corner clearance), oriented to the
    // current rotation, become the fui safe area — so every fui screen's body,
    // list, and popups lay out inside the bezel automatically. Zero on
    // rectangular panels. Insets order is {top, right, bottom, left}.
    int viTop = 0, viRight = 0, viBottom = 0, viLeft = 0;
    renderer.getOrientedViewableTRBL(&viTop, &viRight, &viBottom, &viLeft);
    device.safeArea = Insets{static_cast<int16_t>(viTop), static_cast<int16_t>(viRight),
                             static_cast<int16_t>(viBottom), static_cast<int16_t>(viLeft)};
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

  // Dithered (gray) text needs GfxRenderer::drawTextDither /
  // drawTextRotated90CWDither. Firmwares that predate them still compile:
  // each helper's method-calling overload only instantiates when the renderer
  // has the method (the int/long parameter breaks the tie in its favor);
  // otherwise the fallback overload wins, returns false, and the caller draws
  // solid ink -- the old collapse-to-black behavior. The renderer type must
  // arrive as a template parameter so the lookup is dependent; naming
  // GfxRenderer directly would hard-require the newer API again.
  template <typename R>
  static auto tryDrawTextDither(const R& r, const int fontId, const int x, const int y, const char* line,
                                const ::Color color, const EpdFontFamily::Style st, int)
      -> decltype(r.drawTextDither(fontId, x, y, line, color, st), true) {
    r.drawTextDither(fontId, x, y, line, color, st);
    return true;
  }
  template <typename R>
  static bool tryDrawTextDither(const R&, int, int, int, const char*, ::Color, EpdFontFamily::Style, long) {
    return false;
  }
  template <typename R>
  static auto tryDrawTextRotatedDither(const R& r, const int fontId, const int x, const int y, const char* line,
                                       const ::Color color, const EpdFontFamily::Style st, int)
      -> decltype(r.drawTextRotated90CWDither(fontId, x, y, line, color, st), true) {
    r.drawTextRotated90CWDither(fontId, x, y, line, color, st);
    return true;
  }
  template <typename R>
  static bool tryDrawTextRotatedDither(const R&, int, int, int, const char*, ::Color, EpdFontFamily::Style, long) {
    return false;
  }

  void text(const Rect rect, const char* text, const TextStyle style) override {
    if (!text || rect.empty()) return;
    const int fontId = gfxFont(style.font);
    const EpdFontFamily::Style epdStyle = fontStyle(style);
    // A solid foreground maps to the legacy 1-bit `black` flag; a dithered
    // foreground (a disabled row's dither(LightGray)) is passed through to the
    // renderer's dithered text path so it renders gray instead of solid black.
    // `inverted` marks paper-colored text (textStyleWithForeground sets it
    // alongside a White color for labels on filled elements); honor it as
    // "draw paper", NOT as a color flip -- flipping the White-and-inverted
    // pair lands back on black and paints filled tiles' labels invisible.
    const Color inkColor = style.inverted ? Color::White : style.color;
    const bool black = inkColor == Color::Black;
    const bool dithered = inkColor != Color::Black && inkColor != Color::White;
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
        if (dithered && tryDrawTextRotatedDither(renderer, fontId, x, y, textLine.c_str(), gfxColor(inkColor),
                                                 epdStyle, 0)) {
          return;
        }
        renderer.drawTextRotated90CW(fontId, x, y, textLine.c_str(), black || dithered, epdStyle);
        return;
      }
    }

    const auto drawAligned = [&](const char* textLine, const int y) {
      int x = rect.x;
      if (style.align != TextAlign::Left) {
        const int textW = renderer.getTextWidth(fontId, textLine, epdStyle);
        x = style.align == TextAlign::Center ? rect.x + (rect.width - textW) / 2 : rect.x + rect.width - textW;
        if (x < rect.x) x = rect.x;
      }
      if (dithered && tryDrawTextDither(renderer, fontId, x, y, textLine, gfxColor(inkColor), epdStyle, 0)) {
        return;
      }
      renderer.drawText(fontId, x, y, textLine, black || dithered, epdStyle);
    };

    // Fast path for the common case: text that already fits on one line draws
    // straight from the caller's buffer after a single measure — no truncation
    // string, and for maxLines > 1 no word-splitting wrap walk (whose per-word
    // re-measures made every wrapped-capable label pay for wrapping it never
    // needed). Matters on e-paper list screens that rebuild every row per
    // repaint.
    if (renderer.getTextWidth(fontId, text, epdStyle) <= rect.width) {
      drawAligned(text, rect.y + std::max(0, (rect.height - lh) / 2));
      return;
    }

    if (maxLines == 1) {
      const std::string textLine = renderer.truncatedText(fontId, text, rect.width, epdStyle);
      drawAligned(textLine.c_str(), rect.y + std::max(0, (rect.height - lh) / 2));
      return;
    }

    const std::vector<std::string> lines = renderer.wrappedText(fontId, text, rect.width, maxLines, epdStyle);
    const int blockH = static_cast<int>(lines.size()) * lh;
    int y = rect.y + std::max(0, (rect.height - blockH) / 2);
    for (const auto& textLine : lines) {
      drawAligned(textLine.c_str(), y);
      y += lh;
    }
  }

  void bitmap(const Rect rect, const BitmapRef bitmap, const BitmapMode mode,
              const Paint foreground = Paint::solid(Color::Black),
              const Rotation rotation = Rotation::None) override {
    if (!bitmap || rect.empty()) return;

    // BW1 (set bit = ink) and Mask1 (Icon convention: bit 0 = ink), both
    // row-major natural orientation — same contract as DisplayTarget, so one
    // BitmapRef renders identically on either target. The SDK's shared
    // sampling helper folds the Mask1 polarity and handles every BitmapMode
    // (including Stretch/Contain/Cover scaling and tiling); each ink pixel
    // lands via drawPixel — adequate for icons and pattern fills on a 1-bit
    // panel. Note: GfxRenderer::drawIcon's pre-rotated asset layout is NOT
    // this contract; convert such assets before wrapping them in a BitmapRef.
    if (bitmap.format != BitmapFormat::BW1 && bitmap.format != BitmapFormat::Mask1) return;
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
      // Frame stores a const DeviceContext&; keep the context as a member so
      // it outlives the frame (a deviceContext() temporary would dangle and
      // corrupt every registered hit rect).
      : target(renderer), device(target.deviceContext()), frame(target, device, input, interactions) {
    target.setFont(GfxRendererTarget::FONT_SMALL, smallFontId);
    target.setFont(GfxRendererTarget::FONT_BODY, bodyFontId);
    target.setFont(GfxRendererTarget::FONT_TITLE, titleFontId);
  }

  GfxRendererTarget target;
  DeviceContext device;
  InputSnapshot input{};
  InteractionBuffer<MaxInteractions> interactions;
  Frame<MaxInteractions> frame;
};

}  // namespace ui
}  // namespace freeink

#endif  // __has_include(<GfxRenderer.h>)
