#pragma once

// Native framebuffer DrawTarget — renders FreeInkUI with NO external graphics
// library. This is the default render path for SDK apps: it needs only a raw
// 1-bit framebuffer you already own (e.g. FreeInkDisplay::getFrameBuffer()), so
// nothing here depends on CrossPoint's GfxRenderer (that adapter,
// FreeInkUIGfxRenderer.h, stays an opt-in for firmwares that ship GfxRenderer).
// Being dependency-free, the exact same drawing code runs in host unit tests.
//
// Framebuffer convention (matches FreeInkDisplay): 1bpp, MSB-first (bit 7 is the
// leftmost pixel), row-major, `widthBytes` bytes per row. A SET bit is WHITE, a
// CLEAR bit is BLACK ink — i.e. clearScreen(0xFF) yields a white page.
//
// Text uses the bundled Noto Sans bitmap font (FreeInkUIFont.h). Every font
// slot defaults to it; call setFont() to swap in your own BitmapFont (see
// docs/freeink-ui.md and tools/gen_font.py for generating one from any TTF).
// Gray paints are reproduced on the 1-bit panel with an ordered Bayer dither.

#include <FreeInkUI.h>
#include <FreeInkUIFont.h>

namespace freeink {
namespace ui {

class DisplayTarget final : public DrawTarget {
 public:
  // Logical font slots. ThemeTokens.fontSmall/Body/Title are just FontId values
  // the app assigns; they index these slots. All default to the bundled font.
  static constexpr FontId FONT_SLOTS = 8;

  DisplayTarget(uint8_t* framebuffer, int16_t width, int16_t height, int16_t widthBytes)
      : fb_(framebuffer), w_(width), h_(height), wb_(widthBytes) {
    for (FontId i = 0; i < FONT_SLOTS; ++i) fonts_[i] = &kNotoSansFont;
  }

  // Point one slot, or every slot, at a different BitmapFont.
  void setFont(const FontId slot, const BitmapFont& font) {
    if (slot < FONT_SLOTS) fonts_[slot] = &font;
  }
  void setFont(const BitmapFont& font) {
    for (FontId i = 0; i < FONT_SLOTS; ++i) fonts_[i] = &font;
  }

  // Convenience: a DeviceContext sized to this framebuffer. Pass the device's
  // logical orientation (the framebuffer is already in that orientation).
  DeviceContext deviceContext(const Orientation orientation = Orientation::Portrait) const {
    DeviceContext device;
    device.width = w_;
    device.height = h_;
    device.orientation = orientation;
    device.hasButtons = true;
    return device;
  }

  Size measureText(const FontId font, const char* text, const TextStyle) const override {
    const BitmapFont& f = fontFor(font);
    if (!text) return Size{0, static_cast<int16_t>(f.yAdvance)};
    int16_t width = 0;
    const char* p = text;
    while (*p) {
      uint32_t cp;
      p += decodeUtf8(p, cp);
      width = static_cast<int16_t>(width + runAdvance(f, cp));
    }
    return Size{width, static_cast<int16_t>(f.yAdvance)};
  }

  int16_t lineHeight(const FontId font) const override { return static_cast<int16_t>(fontFor(font).yAdvance); }

  void fill(const Rect rect, const Paint paint, const uint8_t radius = 0,
            const uint8_t corners = CornersAll) override {
    if (rect.empty() || paint.kind == PaintKind::None) return;
    if (paint.kind == PaintKind::Bitmap) {
      bitmap(rect, paint.bitmap.bitmap, paint.bitmap.mode, Paint::solid(Color::Black));
      return;
    }
    if (paint.color == Color::Transparent) return;
    for (int16_t y = rect.y; y < rect.bottom(); ++y) {
      for (int16_t x = rect.x; x < rect.right(); ++x) {
        if (radius > 0 && !insideRounded(rect, radius, corners, x, y)) continue;
        plot(x, y, paint.color);
      }
    }
  }

  void stroke(const Rect rect, const Paint paint, const uint8_t width, const uint8_t radius = 0,
              const uint8_t corners = CornersAll) override {
    if (rect.empty() || width == 0 || paint.kind == PaintKind::None || paint.color == Color::Transparent) return;
    const Rect inner = rect.inset(Insets{static_cast<int16_t>(width), static_cast<int16_t>(width),
                                         static_cast<int16_t>(width), static_cast<int16_t>(width)});
    for (int16_t y = rect.y; y < rect.bottom(); ++y) {
      for (int16_t x = rect.x; x < rect.right(); ++x) {
        const bool onOuter = radius == 0 || insideRounded(rect, radius, corners, x, y);
        if (!onOuter) continue;
        const bool inHole = !inner.empty() && (radius == 0 ? inner.contains(x, y)
                                                           : insideRounded(inner, radius, corners, x, y));
        if (!inHole) plot(x, y, paint.color);
      }
    }
  }

  void line(const Point from, const Point to, const uint8_t width, const Paint paint) override {
    if (paint.kind == PaintKind::None || paint.color == Color::Transparent) return;
    int x0 = from.x, y0 = from.y;
    const int x1 = to.x, y1 = to.y;
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    const int half = width / 2;
    while (true) {
      for (int by = -half; by <= half; ++by)
        for (int bx = -half; bx <= half; ++bx)
          plot(static_cast<int16_t>(x0 + bx), static_cast<int16_t>(y0 + by), paint.color);
      if (x0 == x1 && y0 == y1) break;
      const int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  void triangle(const Point a, const Point b, const Point c, const Paint paint) override {
    if (paint.kind == PaintKind::None || paint.color == Color::Transparent) return;
    const int16_t minX = min3(a.x, b.x, c.x), maxX = max3(a.x, b.x, c.x);
    const int16_t minY = min3(a.y, b.y, c.y), maxY = max3(a.y, b.y, c.y);
    for (int16_t y = minY; y <= maxY; ++y) {
      for (int16_t x = minX; x <= maxX; ++x) {
        if (inTriangle(x, y, a, b, c)) plot(x, y, paint.color);
      }
    }
  }

  void text(const Rect rect, const char* text, const TextStyle style) override {
    if (!text || rect.empty()) return;
    const BitmapFont& f = fontFor(style.font);
    const bool ink = !style.inverted && style.color != Color::White;
    layoutText(*this, rect, text, style, [&](const char* line, const Rect lineRect) {
      drawRun(f, line, lineRect.x, static_cast<int16_t>(lineRect.y + f.ascent), ink);
    });
  }

  void bitmap(const Rect rect, const BitmapRef bitmap, const BitmapMode mode,
              const Paint foreground = Paint::solid(Color::Black),
              const Rotation rotation = Rotation::None) override {
    if (!bitmap || rect.empty()) return;
    // BW1 and Mask1 are both row-major, MSB-first, set-bit-is-ink masks here.
    if (bitmap.format != BitmapFormat::BW1 && bitmap.format != BitmapFormat::Mask1) return;
    const Color color = foreground.kind == PaintKind::None ? Color::Black : foreground.color;
    if (color == Color::Transparent) return;
    forEachBitmapPixel(
        rect, bitmap, mode, [&](const int16_t px, const int16_t py) { plot(px, py, color); }, rotation);
  }

 private:
  uint8_t* fb_;
  int16_t w_;
  int16_t h_;
  int16_t wb_;
  const BitmapFont* fonts_[FONT_SLOTS];

  const BitmapFont& fontFor(const FontId slot) const { return *fonts_[slot < FONT_SLOTS ? slot : 0]; }

  // 4x4 ordered Bayer matrix (0..15) — reproduces gray levels on a 1-bit panel.
  static bool inkForColor(const Color color, const int16_t x, const int16_t y) {
    static constexpr uint8_t kBayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    switch (color) {
      case Color::Black:
        return true;
      case Color::DarkGray:
        return kBayer[y & 3][x & 3] < 12;  // ~75% ink
      case Color::LightGray:
        return kBayer[y & 3][x & 3] < 4;  // ~25% ink
      case Color::White:
      case Color::Transparent:
      default:
        return false;
    }
  }

  void plot(const int16_t x, const int16_t y, const Color color) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_ || color == Color::Transparent) return;
    uint8_t& byte = fb_[static_cast<int32_t>(y) * wb_ + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
    if (inkForColor(color, x, y))
      byte = static_cast<uint8_t>(byte & ~mask);  // ink: clear bit -> black
    else if (color == Color::White)
      byte = static_cast<uint8_t>(byte | mask);  // explicit white: set bit
    // gray "off" pixels leave the background untouched (dither shows through)
  }

  // True if (x,y) is inside `rect` once `radius` rounds the selected corners.
  static bool insideRounded(const Rect rect, const uint8_t radius, const uint8_t corners, const int16_t x,
                            const int16_t y) {
    if (x < rect.x || y < rect.y || x >= rect.right() || y >= rect.bottom()) return false;
    int16_t r = radius;
    const int16_t halfW = static_cast<int16_t>(rect.width / 2);
    const int16_t halfH = static_cast<int16_t>(rect.height / 2);
    if (r > halfW) r = halfW;
    if (r > halfH) r = halfH;
    if (r <= 0) return true;
    const bool left = x < rect.x + r;
    const bool right = x >= rect.right() - r;
    const bool top = y < rect.y + r;
    const bool bottom = y >= rect.bottom() - r;
    int16_t cx = 0, cy = 0;
    bool inCorner = false;
    if (left && top && (corners & CornerTopLeft)) { cx = rect.x + r; cy = rect.y + r; inCorner = true; }
    else if (right && top && (corners & CornerTopRight)) { cx = rect.right() - 1 - r; cy = rect.y + r; inCorner = true; }
    else if (left && bottom && (corners & CornerBottomLeft)) { cx = rect.x + r; cy = rect.bottom() - 1 - r; inCorner = true; }
    else if (right && bottom && (corners & CornerBottomRight)) { cx = rect.right() - 1 - r; cy = rect.bottom() - 1 - r; inCorner = true; }
    if (!inCorner) return true;
    const int dxp = x - cx, dyp = y - cy;
    return dxp * dxp + dyp * dyp <= r * r;
  }

  // --- text ------------------------------------------------------------------
  // Decode one UTF-8 codepoint; returns bytes consumed (>=1).
  static int decodeUtf8(const char* s, uint32_t& cp) {
    const uint8_t c0 = static_cast<uint8_t>(s[0]);
    if (c0 < 0x80) { cp = c0; return 1; }
    if ((c0 & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
      cp = ((c0 & 0x1Fu) << 6) | (static_cast<uint8_t>(s[1]) & 0x3Fu);
      return 2;
    }
    if ((c0 & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
      cp = ((c0 & 0x0Fu) << 12) | ((static_cast<uint8_t>(s[1]) & 0x3Fu) << 6) | (static_cast<uint8_t>(s[2]) & 0x3Fu);
      return 3;
    }
    if ((c0 & 0xF8) == 0xF0) { cp = '?'; return 4; }
    cp = c0;
    return 1;
  }

  static const FontGlyph* glyphFor(const BitmapFont& f, const uint32_t cp) {
    if (cp < f.first || cp > f.last) return nullptr;
    return &f.glyphs[cp - f.first];
  }

  // Pen advance for a codepoint, mapping the ellipsis to three dots and unknown
  // codepoints to a space — consistent with how drawRun() renders them.
  static int16_t runAdvance(const BitmapFont& f, const uint32_t cp) {
    if (cp == 0x2026) {  // U+2026 HORIZONTAL ELLIPSIS -> "..."
      const FontGlyph* dot = glyphFor(f, '.');
      return static_cast<int16_t>(dot ? dot->xAdvance * 3 : 0);
    }
    const FontGlyph* g = glyphFor(f, cp);
    if (g) return g->xAdvance;
    const FontGlyph* sp = glyphFor(f, ' ');
    return static_cast<int16_t>(sp ? sp->xAdvance : 0);
  }

  void drawGlyph(const BitmapFont& f, const FontGlyph& g, const int16_t penX, const int16_t baseline,
                 const bool ink) {
    const Color color = ink ? Color::Black : Color::White;
    const int bytesPerRow = (g.width + 7) / 8;
    for (int gy = 0; gy < g.height; ++gy) {
      for (int gx = 0; gx < g.width; ++gx) {
        const uint8_t byte = f.bitmap[g.bitmapOffset + gy * bytesPerRow + (gx >> 3)];
        if ((byte >> (7 - (gx & 7))) & 0x01) {
          plot(static_cast<int16_t>(penX + g.xOffset + gx), static_cast<int16_t>(baseline + g.yOffset + gy), color);
        }
      }
    }
  }

  void drawRun(const BitmapFont& f, const char* s, int16_t penX, const int16_t baseline, const bool ink) {
    while (*s) {
      uint32_t cp;
      s += decodeUtf8(s, cp);
      if (cp == 0x2026) {
        const FontGlyph* dot = glyphFor(f, '.');
        if (dot) for (int i = 0; i < 3; ++i) { drawGlyph(f, *dot, penX, baseline, ink); penX += dot->xAdvance; }
        continue;
      }
      const FontGlyph* g = glyphFor(f, cp);
      if (g) { drawGlyph(f, *g, penX, baseline, ink); penX = static_cast<int16_t>(penX + g->xAdvance); }
      else { penX = static_cast<int16_t>(penX + runAdvance(f, cp)); }
    }
  }

  static int abs(const int v) { return v < 0 ? -v : v; }
  static int16_t min3(const int16_t a, const int16_t b, const int16_t c) {
    const int16_t m = a < b ? a : b;
    return m < c ? m : c;
  }
  static int16_t max3(const int16_t a, const int16_t b, const int16_t c) {
    const int16_t m = a > b ? a : b;
    return m > c ? m : c;
  }
  static bool inTriangle(const int16_t px, const int16_t py, const Point a, const Point b, const Point c) {
    const int32_t d1 = edge(px, py, a, b);
    const int32_t d2 = edge(px, py, b, c);
    const int32_t d3 = edge(px, py, c, a);
    const bool hasNeg = d1 < 0 || d2 < 0 || d3 < 0;
    const bool hasPos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(hasNeg && hasPos);  // all same sign (or on an edge) => inside
  }
  static int32_t edge(const int16_t px, const int16_t py, const Point a, const Point b) {
    return static_cast<int32_t>(px - b.x) * (a.y - b.y) - static_cast<int32_t>(a.x - b.x) * (py - b.y);
  }
};

}  // namespace ui
}  // namespace freeink
