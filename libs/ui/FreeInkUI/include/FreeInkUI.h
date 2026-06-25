#pragma once

// FreeInk SDK — lightweight UI primitives.
//
// FreeInkUI is intentionally not a retained DOM. It provides small value types,
// fixed-capacity interaction routing, and simple row/column slot layout so apps
// can build reusable e-paper components without hardcoding panel coordinates.
// Labels and asset pointers are borrowed; the caller owns lifetime.
//
// The library is freestanding C++ (no Arduino/ESP-IDF dependency) so the same
// code runs on firmware and in host-side unit tests.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace ui {

using ActionId = uint16_t;
static constexpr ActionId NO_ACTION = 0;

struct Size {
  int16_t width = 0;
  int16_t height = 0;
};

struct Point {
  int16_t x = 0;
  int16_t y = 0;
};

struct Insets {
  int16_t top = 0;
  int16_t right = 0;
  int16_t bottom = 0;
  int16_t left = 0;
};

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;

  bool empty() const { return width <= 0 || height <= 0; }
  int16_t right() const { return static_cast<int16_t>(x + width); }
  int16_t bottom() const { return static_cast<int16_t>(y + height); }
  bool contains(int16_t px, int16_t py) const { return px >= x && py >= y && px < right() && py < bottom(); }
  Rect inset(Insets insets) const {
    return Rect{static_cast<int16_t>(x + insets.left), static_cast<int16_t>(y + insets.top),
                static_cast<int16_t>(width - insets.left - insets.right),
                static_cast<int16_t>(height - insets.top - insets.bottom)};
  }
};

enum class Orientation : uint8_t {
  Portrait,
  LandscapeClockwise,
  PortraitInverted,
  LandscapeCounterClockwise,
};

struct DeviceContext {
  int16_t width = 0;
  int16_t height = 0;
  Orientation orientation = Orientation::Portrait;
  bool hasTouch = false;
  bool hasButtons = true;
  Insets safeArea{};
  int16_t minTouchSize = 44;

  Rect screen() const { return Rect{0, 0, width, height}; }
  Rect safeRect() const { return screen().inset(safeArea); }
};

// Maps a touch point reported in normalized panel-native portrait
// coordinates (0..1, as the input layer reports taps) to logical screen
// coordinates under the device's orientation — the transform every app
// otherwise re-derives by hand. flipX/flipY compensate for mirrored panel
// mounting; they are a property of the board, not the app, so feed them from
// the board profile or a config constant.
inline Point touchToLogical(const DeviceContext& device, float nx, float ny, const bool flipX = false,
                            const bool flipY = false) {
  if (flipX) nx = 1.0f - nx;
  if (flipY) ny = 1.0f - ny;
  float lx;
  float ly;
  switch (device.orientation) {
    case Orientation::PortraitInverted:
      lx = 1.0f - nx;
      ly = 1.0f - ny;
      break;
    case Orientation::LandscapeClockwise:
      lx = 1.0f - ny;
      ly = nx;
      break;
    case Orientation::LandscapeCounterClockwise:
      lx = ny;
      ly = 1.0f - nx;
      break;
    case Orientation::Portrait:
    default:
      lx = nx;
      ly = ny;
      break;
  }
  int32_t x = static_cast<int32_t>(lx * device.width);
  int32_t y = static_cast<int32_t>(ly * device.height);
  if (x < 0) x = 0;
  if (x >= device.width) x = device.width - 1;
  if (y < 0) y = 0;
  if (y >= device.height) y = device.height - 1;
  return Point{static_cast<int16_t>(x), static_cast<int16_t>(y)};
}

enum State : uint8_t {
  StateNormal = 0,
  StateSelected = 1 << 0,
  StateFocused = 1 << 1,
  StateActive = 1 << 2,
  StateDisabled = 1 << 3,
  StateChecked = 1 << 4,
};

inline State operator|(State a, State b) {
  return static_cast<State>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline State operator&(State a, State b) {
  return static_cast<State>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline State& operator|=(State& a, State b) {
  a = a | b;
  return a;
}
inline bool hasState(State state, State bit) { return (static_cast<uint8_t>(state) & static_cast<uint8_t>(bit)) != 0; }

enum InputMask : uint16_t {
  InputNone = 0,
  InputTouch = 1 << 0,
  InputFocus = 1 << 1,
  InputConfirm = 1 << 2,
  InputBack = 1 << 3,
  InputPrev = 1 << 4,
  InputNext = 1 << 5,
  InputSwipeLeft = 1 << 6,
  InputSwipeRight = 1 << 7,
  InputLongPress = 1 << 8,
  InputDefault = InputTouch | InputFocus | InputConfirm,
};

inline InputMask operator|(InputMask a, InputMask b) {
  return static_cast<InputMask>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline bool acceptsInput(uint16_t mask, InputMask bit) { return (mask & static_cast<uint16_t>(bit)) != 0; }

enum class Color : uint8_t {
  Transparent,
  White,
  LightGray,
  DarkGray,
  Black,
};

enum class PaintKind : uint8_t {
  None,
  Solid,
  Dither,
  Bitmap,
};

enum class BitmapFormat : uint8_t {
  Mask1,
  BW1,
  Gray2,
  NativeBmp,
};

enum class BitmapMode : uint8_t {
  Center,
  Stretch,
  Contain,
  Cover,
  Tile,
  TileX,
  TileY,
};

struct BitmapRef {
  const uint8_t* data = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;
  BitmapFormat format = BitmapFormat::BW1;
  bool progmem = true;

  explicit operator bool() const { return data != nullptr && width > 0 && height > 0; }
};

struct AssetRef {
  const char* id = nullptr;
  const char* path = nullptr;
  BitmapRef bitmap{};

  explicit operator bool() const { return bitmap || path != nullptr || id != nullptr; }
};

class AssetResolver {
 public:
  virtual ~AssetResolver() = default;
  virtual BitmapRef bitmapFor(const AssetRef& asset) = 0;
};

struct BitmapFill {
  BitmapRef bitmap{};
  BitmapMode mode = BitmapMode::Center;
};

// Per-element rotation (side-bezel button hints, vertical labels). Targets
// declare what they support; unsupported rotations draw unrotated.
enum class Rotation : uint8_t {
  None,
  CW90,
  R180,
  CCW90,
};

// Visits every ink pixel of a row-major, MSB-first, set-bit-is-ink 1-bit mask
// laid out into rect under the given mode (Center at native size, Stretch /
// Contain / Cover with nearest-neighbor sampling, Tile / TileX / TileY
// repeating), optionally rotated. Output is clipped to the rect. Shared,
// host-testable layout math for DrawTarget implementations: plot(x, y) is
// called per ink pixel. Rotation here is per-element (an icon rotated
// relative to its screen); whole-display rotation happens in the renderer's
// coordinate mapping and needs nothing from callers.
template <typename Plot>
inline void forEachBitmapPixel(const Rect rect, const BitmapRef& src, const BitmapMode mode, Plot&& plot,
                               const Rotation rotation = Rotation::None) {
  if (!src || rect.empty()) return;
  const int bytesPerRow = (src.width + 7) / 8;
  // Mask1 is the freeink::Icon convention (bit 1 = transparent, bit 0 = draw);
  // BW1 is set-bit-is-ink. Fold that polarity in here so both formats plot
  // their drawn pixels through the same sampling math.
  const bool maskInverted = src.format == BitmapFormat::Mask1;
  const auto srcInk = [&](const int sx, const int sy) {
    const uint8_t bit = (src.data[sy * bytesPerRow + sx / 8] >> (7 - (sx % 8))) & 0x01;
    return static_cast<uint8_t>(maskInverted ? (bit ^ 1) : bit);
  };

  // Sample through the rotation so the rest of the layout math sees a
  // pre-rotated bitmap of the effective dimensions.
  struct {
    int16_t width;
    int16_t height;
  } bitmap{static_cast<int16_t>(src.width), static_cast<int16_t>(src.height)};
  if (rotation == Rotation::CW90 || rotation == Rotation::CCW90) {
    bitmap.width = static_cast<int16_t>(src.height);
    bitmap.height = static_cast<int16_t>(src.width);
  }
  const auto inkAt = [&](const int sx, const int sy) {
    switch (rotation) {
      case Rotation::CW90:
        return srcInk(sy, src.height - 1 - sx);
      case Rotation::R180:
        return srcInk(src.width - 1 - sx, src.height - 1 - sy);
      case Rotation::CCW90:
        return srcInk(src.width - 1 - sy, sx);
      case Rotation::None:
      default:
        return srcInk(sx, sy);
    }
  };

  if (mode == BitmapMode::Tile || mode == BitmapMode::TileX || mode == BitmapMode::TileY) {
    const bool tileX = mode != BitmapMode::TileY;
    const bool tileY = mode != BitmapMode::TileX;
    const int spanX = tileX ? rect.width : (bitmap.width < rect.width ? bitmap.width : rect.width);
    const int spanY = tileY ? rect.height : (bitmap.height < rect.height ? bitmap.height : rect.height);
    for (int dy = 0; dy < spanY; ++dy) {
      for (int dx = 0; dx < spanX; ++dx) {
        if (inkAt(dx % bitmap.width, dy % bitmap.height)) {
          plot(static_cast<int16_t>(rect.x + dx), static_cast<int16_t>(rect.y + dy));
        }
      }
    }
    return;
  }

  int dstW = bitmap.width;
  int dstH = bitmap.height;
  if (mode == BitmapMode::Stretch) {
    dstW = rect.width;
    dstH = rect.height;
  } else if (mode == BitmapMode::Contain || mode == BitmapMode::Cover) {
    const int32_t byW = (static_cast<int32_t>(rect.width) << 8) / bitmap.width;
    const int32_t byH = (static_cast<int32_t>(rect.height) << 8) / bitmap.height;
    const int32_t scale = mode == BitmapMode::Contain ? (byW < byH ? byW : byH) : (byW > byH ? byW : byH);
    dstW = static_cast<int>((bitmap.width * scale) >> 8);
    dstH = static_cast<int>((bitmap.height * scale) >> 8);
  }
  if (dstW <= 0 || dstH <= 0) return;
  const int x0 = rect.x + (rect.width - dstW) / 2;
  const int y0 = rect.y + (rect.height - dstH) / 2;
  for (int dy = 0; dy < dstH; ++dy) {
    const int py = y0 + dy;
    if (py < rect.y || py >= rect.bottom()) continue;  // Cover overflow clips to the rect
    const int sy = static_cast<int>((static_cast<int32_t>(dy) * bitmap.height) / dstH);
    for (int dx = 0; dx < dstW; ++dx) {
      const int px = x0 + dx;
      if (px < rect.x || px >= rect.right()) continue;
      const int sx = static_cast<int>((static_cast<int32_t>(dx) * bitmap.width) / dstW);
      if (inkAt(sx, sy)) plot(static_cast<int16_t>(px), static_cast<int16_t>(py));
    }
  }
}

struct Paint {
  PaintKind kind = PaintKind::None;
  Color color = Color::Transparent;
  BitmapFill bitmap{};

  static Paint none() { return Paint{}; }
  static Paint solid(Color c) {
    Paint p;
    p.kind = PaintKind::Solid;
    p.color = c;
    return p;
  }
  static Paint dither(Color c) {
    Paint p;
    p.kind = PaintKind::Dither;
    p.color = c;
    return p;
  }
  static Paint bitmapFill(BitmapFill fill) {
    Paint p;
    p.kind = PaintKind::Bitmap;
    p.bitmap = fill;
    return p;
  }
};

using FontId = uint16_t;

enum class TextAlign : uint8_t {
  Left,
  Center,
  Right,
};

struct TextStyle {
  FontId font = 0;
  TextAlign align = TextAlign::Left;
  Color color = Color::Black;
  uint8_t maxLines = 1;
  bool bold = false;
  bool inverted = false;
  // Rotated text is single-line; the rect is interpreted in screen space and
  // the text flows along the rotated axis.
  Rotation rotation = Rotation::None;
};

// Which corners a radius applies to (RoundedRaff-style cards round only the
// top band's top corners and the bottom band's bottom corners).
enum Corners : uint8_t {
  CornerTopLeft = 1 << 0,
  CornerTopRight = 1 << 1,
  CornerBottomLeft = 1 << 2,
  CornerBottomRight = 1 << 3,
  CornersTop = CornerTopLeft | CornerTopRight,
  CornersBottom = CornerBottomLeft | CornerBottomRight,
  CornersAll = CornersTop | CornersBottom,
};

enum Edges : uint8_t {
  EdgesNone = 0,
  EdgeTop = 1 << 0,
  EdgeRight = 1 << 1,
  EdgeBottom = 1 << 2,
  EdgeLeft = 1 << 3,
  EdgesHorizontal = EdgeTop | EdgeBottom,
  EdgesVertical = EdgeLeft | EdgeRight,
  EdgesAll = EdgesHorizontal | EdgesVertical,
};

struct BoxStyle {
  Paint background = Paint::none();
  Paint foreground = Paint::solid(Color::Black);
  Paint border = Paint::none();
  uint8_t borderWidth = 0;
  uint8_t radius = 0;
  uint8_t corners = CornersAll;
};

struct StyleSet {
  BoxStyle normal{};
  BoxStyle selected{};
  BoxStyle focused{};
  BoxStyle active{};
  BoxStyle disabled{};

  // True when the caller never assigned any visible style, so components fall
  // back to their built-in defaults. Borders count too: an outline-only style
  // (transparent background) is still "set".
  bool unset() const {
    return normal.background.kind == PaintKind::None && normal.border.kind == PaintKind::None &&
           selected.background.kind == PaintKind::None && focused.background.kind == PaintKind::None;
  }

  const BoxStyle& resolve(State state) const {
    if (hasState(state, StateDisabled)) return disabled;
    if (hasState(state, StateActive)) return active;
    if (hasState(state, StateFocused)) return focused;
    if (hasState(state, StateSelected) || hasState(state, StateChecked)) return selected;
    return normal;
  }
};

struct ThemeTokens {
  FontId fontSmall = 0;
  FontId fontBody = 0;
  FontId fontTitle = 0;
  int16_t spaceXs = 2;
  int16_t spaceSm = 4;
  int16_t spaceMd = 8;
  int16_t spaceLg = 16;
  int16_t minTouchSize = 44;
  int16_t rowHeight = 44;
  int16_t headerHeight = 44;
  int16_t footerHeight = 40;
  int16_t progressHeight = 4;
  TextStyle smallText{};
  TextStyle bodyText{};
  TextStyle titleText{};
  StyleSet button{};
  StyleSet listRow{};
  StyleSet key{};
  StyleSet popup{};
  StyleSet textField{};
};

struct ThemeDocument {
  uint8_t schema = 0;
  const char* id = nullptr;
  const char* name = nullptr;
  const char* deviceId = nullptr;
  ThemeTokens tokens{};
  AssetResolver* assets = nullptr;
};

class DrawTarget {
 public:
  virtual ~DrawTarget() = default;
  virtual Size measureText(FontId font, const char* text, TextStyle style) const = 0;
  virtual int16_t lineHeight(FontId font) const = 0;
  // text() implementations must honor style.align, style.maxLines, and
  // ellipsis truncation. Targets with a native wrapping pipeline (bidi,
  // kerning-aware) should use it; everyone else can delegate the whole
  // algorithm to layoutText() below and only draw the emitted runs.
  // radius > 0 fills a rounded rect (corners selects which); targets without
  // rounded support may ignore both.
  virtual void fill(Rect rect, Paint paint, uint8_t radius = 0, uint8_t corners = CornersAll) = 0;
  virtual void stroke(Rect rect, Paint paint, uint8_t width, uint8_t radius = 0, uint8_t corners = CornersAll) = 0;
  // Straight line segment of the given thickness; underlines, dividers,
  // battery/key glyph art.
  virtual void line(Point from, Point to, uint8_t width, Paint paint) = 0;
  // Filled triangle; selection markers, arrows, bookmark notches.
  virtual void triangle(Point a, Point b, Point c, Paint paint) = 0;
  virtual void text(Rect rect, const char* text, TextStyle style) = 0;
  // rotation is per-element (relative to the screen); whole-display rotation
  // is the renderer's coordinate mapping and needs nothing here.
  virtual void bitmap(Rect rect, BitmapRef bitmap, BitmapMode mode, Paint foreground = Paint::solid(Color::Black),
                      Rotation rotation = Rotation::None) = 0;
};

// UTF-8 horizontal ellipsis, appended to truncated lines.
static constexpr const char* TEXT_ELLIPSIS = "\xE2\x80\xA6";

// SDK-owned text layout: greedy word wrap up to style.maxLines, hard breaks
// on '\n', character-level breaking for words wider than the rect, ellipsis
// on the last line when text remains, alignment, and vertical centering of
// the line block. Built only on measureText/lineHeight, so a DrawTarget's
// text() can delegate everything here and just draw single-line runs:
//
//   void text(Rect rect, const char* text, TextStyle style) override {
//     layoutText(*this, rect, text, style, [&](const char* line, Rect r) {
//       rawDrawLine(r.x, r.y, line, style);  // line is NUL-terminated
//     });
//   }
//
// emit(line, lineRect) is called per line; `line` points to an internal
// buffer valid only during the call. lineRect is the aligned rect of the
// measured run (height = lineHeight). Lines cap at 16 and ~220 bytes; both
// are far beyond e-paper chrome needs.
template <typename Emit>
inline void layoutText(const DrawTarget& target, const Rect rect, const char* text, const TextStyle style,
                       Emit&& emit) {
  if (!text || text[0] == '\0' || rect.empty()) return;
  constexpr uint8_t MAX_LINES = 16;
  constexpr uint16_t MAX_LINE_BYTES = 220;
  const uint8_t maxLines = style.maxLines > 0 ? (style.maxLines < MAX_LINES ? style.maxLines : MAX_LINES) : 1;
  const int16_t lh = target.lineHeight(style.font);

  char buf[MAX_LINE_BYTES + 8];
  const auto widthOf = [&](const char* start, const uint16_t len, const bool ellipsis) {
    uint16_t n = len < MAX_LINE_BYTES ? len : MAX_LINE_BYTES;
    for (uint16_t i = 0; i < n; ++i) buf[i] = start[i];
    if (ellipsis) {
      buf[n++] = '\xE2';
      buf[n++] = '\x80';
      buf[n++] = '\xA6';
    }
    buf[n] = '\0';
    return target.measureText(style.font, buf, style).width;
  };

  // Pass 1: split into line ranges.
  struct Range {
    const char* begin;
    uint16_t len;
    bool ellipsis;
  };
  Range ranges[MAX_LINES];
  uint8_t lineCount = 0;
  const char* p = text;
  while (*p != '\0' && lineCount < maxLines) {
    while (*p == ' ') ++p;  // lines never start with spaces
    if (*p == '\0') break;
    if (*p == '\n') {
      ++p;
      ranges[lineCount++] = Range{p, 0, false};  // preserve blank lines
      continue;
    }
    const char* lineStart = p;
    uint16_t fitLen = 0;
    const char* scan = p;
    while (true) {
      // advance to the end of the next word
      const char* wordEnd = scan;
      while (*wordEnd != '\0' && *wordEnd != ' ' && *wordEnd != '\n') ++wordEnd;
      const uint16_t candidate = static_cast<uint16_t>(wordEnd - lineStart);
      if (candidate > MAX_LINE_BYTES || widthOf(lineStart, candidate, false) > rect.width) break;
      fitLen = candidate;
      if (*wordEnd == '\0' || *wordEnd == '\n') break;
      scan = wordEnd + 1;
      while (*scan == ' ') ++scan;
      if (*scan == '\0' || *scan == '\n') break;
    }
    if (fitLen == 0) {
      // single word wider than the rect: break at characters (UTF-8 aware)
      uint16_t len = 0;
      while (lineStart[len] != '\0' && lineStart[len] != ' ' && lineStart[len] != '\n') {
        uint16_t next = static_cast<uint16_t>(len + 1);
        while ((lineStart[next] & 0xC0) == 0x80) ++next;  // keep codepoints whole
        if (next > MAX_LINE_BYTES || (len > 0 && widthOf(lineStart, next, false) > rect.width)) break;
        len = next;
      }
      fitLen = len > 0 ? len : 1;
    }
    ranges[lineCount++] = Range{lineStart, fitLen, false};
    p = lineStart + fitLen;
    if (*p == '\n') ++p;
  }
  if (lineCount == 0) return;

  // Anything left over: ellipsize the last line, shrinking until it fits.
  while (*p == ' ') ++p;
  if (*p != '\0') {
    Range& last = ranges[lineCount - 1];
    last.ellipsis = true;
    while (last.len > 0 && widthOf(last.begin, last.len, true) > rect.width) {
      --last.len;
      while (last.len > 0 && (last.begin[last.len] & 0xC0) == 0x80) --last.len;  // codepoint boundary
    }
  }

  // Pass 2: emit aligned, vertically centered runs.
  const int16_t blockH = static_cast<int16_t>(lineCount * lh);
  int16_t y = static_cast<int16_t>(rect.y + (rect.height > blockH ? (rect.height - blockH) / 2 : 0));
  for (uint8_t i = 0; i < lineCount; ++i) {
    const int16_t w = widthOf(ranges[i].begin, ranges[i].len, ranges[i].ellipsis);  // also fills buf
    int16_t x = rect.x;
    if (style.align == TextAlign::Center) x = static_cast<int16_t>(rect.x + (rect.width - w) / 2);
    if (style.align == TextAlign::Right) x = static_cast<int16_t>(rect.right() - w);
    if (x < rect.x) x = rect.x;
    emit(static_cast<const char*>(buf), Rect{x, y, w, lh});
    y = static_cast<int16_t>(y + lh);
  }
}

// Bounds of `text` wrapped into maxWidth under `style` (honors maxLines and
// the ellipsis tail): width = widest emitted line, height = line count times
// lineHeight. Built on layoutText, so it agrees exactly with what
// text()/drawText render through targets that delegate to it — use it to
// reserve space for wrapped titles, auto-size dialogs, or compute row
// heights, instead of estimating line counts from single-line measureText.
inline Size measureWrappedText(const DrawTarget& target, const char* text, const TextStyle style,
                               const int16_t maxWidth) {
  int16_t widest = 0;
  int16_t lines = 0;
  layoutText(target, Rect{0, 0, maxWidth, 1}, text, style, [&](const char*, const Rect r) {
    if (r.width > widest) widest = r.width;
    ++lines;
  });
  return Size{widest, static_cast<int16_t>(lines * target.lineHeight(style.font))};
}

inline Color invertedColor(const Color color) {
  switch (color) {
    case Color::White:
      return Color::Black;
    case Color::Black:
      return Color::White;
    case Color::LightGray:
      return Color::DarkGray;
    case Color::DarkGray:
      return Color::LightGray;
    case Color::Transparent:
    default:
      return color;
  }
}

inline Paint invertedPaint(Paint paint) {
  if (paint.kind == PaintKind::Solid || paint.kind == PaintKind::Dither) {
    paint.color = invertedColor(paint.color);
  }
  return paint;
}

// Whole-UI dark mode in one call: wrap any DrawTarget and every color drawn
// through it inverts (black↔white, light↔dark gray), including component
// defaults the app never touches. Construct it once over the real target and
// flip setEnabled() from a setting; when disabled it is a pure passthrough.
//
//   freeink::ui::InvertedDrawTarget target(realTarget, settings.darkMode);
//   freeink::ui::Frame<32> ui(target, device, input, interactions);
//
// The screen clear stays app-owned — clear to black when inverted.
class InvertedDrawTarget final : public DrawTarget {
 public:
  explicit InvertedDrawTarget(DrawTarget& inner, bool enabled = true) : inner_(inner), enabled_(enabled) {}

  void setEnabled(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }

  Size measureText(FontId font, const char* text, TextStyle style) const override {
    return inner_.measureText(font, text, style);
  }
  int16_t lineHeight(FontId font) const override { return inner_.lineHeight(font); }
  void fill(Rect rect, Paint paint, uint8_t radius = 0, uint8_t corners = CornersAll) override {
    inner_.fill(rect, enabled_ ? invertedPaint(paint) : paint, radius, corners);
  }
  void stroke(Rect rect, Paint paint, uint8_t width, uint8_t radius = 0, uint8_t corners = CornersAll) override {
    inner_.stroke(rect, enabled_ ? invertedPaint(paint) : paint, width, radius, corners);
  }
  void line(Point from, Point to, uint8_t width, Paint paint) override {
    inner_.line(from, to, width, enabled_ ? invertedPaint(paint) : paint);
  }
  void triangle(Point a, Point b, Point c, Paint paint) override {
    inner_.triangle(a, b, c, enabled_ ? invertedPaint(paint) : paint);
  }
  void text(Rect rect, const char* text, TextStyle style) override {
    if (enabled_) {
      style.color = invertedColor(style.color);
      style.inverted = style.color == Color::White;
    }
    inner_.text(rect, text, style);
  }
  void bitmap(Rect rect, BitmapRef bitmap, BitmapMode mode, Paint foreground = Paint::solid(Color::Black),
              Rotation rotation = Rotation::None) override {
    inner_.bitmap(rect, bitmap, mode, enabled_ ? invertedPaint(foreground) : foreground, rotation);
  }

 private:
  DrawTarget& inner_;
  bool enabled_;
};

struct InputSnapshot {
  bool touchReleased = false;
  bool touchPressed = false;
  bool longPress = false;
  bool swipeLeft = false;
  bool swipeRight = false;
  int16_t touchX = 0;
  int16_t touchY = 0;

  bool focusNext = false;
  bool focusPrev = false;
  bool confirm = false;
  bool back = false;
  bool prev = false;
  bool next = false;
};

struct ActionEvent {
  ActionId action = NO_ACTION;
  int16_t value = 0;
  State state = StateNormal;

  explicit operator bool() const { return action != NO_ACTION; }
};

struct Interaction {
  Rect rect{};
  ActionId action = NO_ACTION;
  int16_t value = 0;
  uint16_t inputMask = InputDefault;
  State state = StateNormal;
  uint8_t focusOrder = 0;
};

class InteractionSink {
 public:
  virtual ~InteractionSink() = default;
  virtual bool addInteraction(const Interaction& interaction) = 0;
  virtual State stateFor(ActionId action, int16_t value, State base) const = 0;
};

template <size_t MaxInteractions>
class InteractionBuffer final : public InteractionSink {
 public:
  bool addInteraction(const Interaction& interaction) override {
    if (interaction.action == NO_ACTION) return false;
    if (count_ >= MaxInteractions) {
      overflowed_ = true;
      return false;
    }
    interactions_[count_++] = interaction;
    return true;
  }

  State stateFor(ActionId action, int16_t value, State base) const override {
    State state = base;
    if (focused_ >= 0 && focused_ < static_cast<int16_t>(count_)) {
      const Interaction& focused = interactions_[focused_];
      if (focused.action == action && focused.value == value) state |= StateFocused;
    }
    if (active_ >= 0 && active_ < static_cast<int16_t>(count_)) {
      const Interaction& active = interactions_[active_];
      if (active.action == action && active.value == value) state |= StateActive;
    }
    return state;
  }

  void clear() {
    count_ = 0;
    overflowed_ = false;
  }

  size_t count() const { return count_; }
  // True if a frame registered more interactions than the buffer holds —
  // dropped elements never receive input, so size the template accordingly.
  bool overflowed() const { return overflowed_; }
  const Interaction* data() const { return interactions_; }
  int16_t focusedIndex() const { return focused_; }
  void setFocusedIndex(int16_t index) {
    if (index >= 0 && index < static_cast<int16_t>(count_))
      focused_ = index;
    else
      focused_ = -1;
  }

  ActionEvent route(const InputSnapshot& input) {
    ActionEvent event{};

    if (input.touchPressed) {
      active_ = findTouch(input.touchX, input.touchY, InputTouch);
    }

    if (input.touchReleased) {
      const int16_t idx = findTouch(input.touchX, input.touchY, input.longPress ? InputLongPress : InputTouch);
      active_ = -1;
      if (idx >= 0) return eventFor(idx);
    }

    if (input.swipeLeft) {
      const int16_t idx = findFirst(InputSwipeLeft);
      if (idx >= 0) return eventFor(idx);
    }
    if (input.swipeRight) {
      const int16_t idx = findFirst(InputSwipeRight);
      if (idx >= 0) return eventFor(idx);
    }
    if (input.back) {
      const int16_t idx = findFirst(InputBack);
      if (idx >= 0) return eventFor(idx);
    }
    if (input.prev) {
      const int16_t idx = findFirst(InputPrev);
      if (idx >= 0) return eventFor(idx);
    }
    if (input.next) {
      const int16_t idx = findFirst(InputNext);
      if (idx >= 0) return eventFor(idx);
    }

    if (input.focusNext) moveFocus(1);
    if (input.focusPrev) moveFocus(-1);
    // Focus indices persist across frames so GPIO navigation survives
    // re-renders, but a screen change can leave a stale index. Only confirm a
    // focus target that exists in the current table and accepts confirm input.
    if (input.confirm && focused_ >= 0 && focused_ < static_cast<int16_t>(count_)) {
      const Interaction& focused = interactions_[focused_];
      if (!hasState(focused.state, StateDisabled) && acceptsInput(focused.inputMask, InputConfirm)) {
        return eventFor(focused_);
      }
    }

    return event;
  }

 private:
  Interaction interactions_[MaxInteractions]{};
  size_t count_ = 0;
  int16_t focused_ = -1;
  int16_t active_ = -1;
  bool overflowed_ = false;

  bool focusable(const Interaction& interaction) const {
    return !hasState(interaction.state, StateDisabled) && acceptsInput(interaction.inputMask, InputFocus);
  }

  int16_t findTouch(int16_t x, int16_t y, InputMask kind) const {
    for (int16_t i = static_cast<int16_t>(count_) - 1; i >= 0; --i) {
      const Interaction& interaction = interactions_[i];
      if (hasState(interaction.state, StateDisabled)) continue;
      const bool acceptsKind = acceptsInput(interaction.inputMask, kind);
      const bool acceptsTouchFallback = kind != InputLongPress && acceptsInput(interaction.inputMask, InputTouch);
      if (!acceptsKind && !acceptsTouchFallback) continue;
      if (interaction.rect.contains(x, y)) return i;
    }
    return -1;
  }

  int16_t findFirst(InputMask kind) const {
    for (int16_t i = 0; i < static_cast<int16_t>(count_); ++i) {
      const Interaction& interaction = interactions_[i];
      if (hasState(interaction.state, StateDisabled)) continue;
      if (acceptsInput(interaction.inputMask, kind)) return i;
    }
    return -1;
  }

  void moveFocus(int8_t delta) {
    if (count_ == 0) {
      focused_ = -1;
      return;
    }
    int16_t start = focused_;
    if (start < 0 || start >= static_cast<int16_t>(count_)) start = delta > 0 ? -1 : static_cast<int16_t>(count_);
    for (size_t step = 0; step < count_; ++step) {
      int16_t idx = static_cast<int16_t>(start + delta);
      if (idx < 0) idx = static_cast<int16_t>(count_ - 1);
      if (idx >= static_cast<int16_t>(count_)) idx = 0;
      if (focusable(interactions_[idx])) {
        focused_ = idx;
        return;
      }
      start = idx;
    }
    focused_ = -1;
  }

  ActionEvent eventFor(int16_t idx) const {
    const Interaction& interaction = interactions_[idx];
    return ActionEvent{interaction.action, interaction.value, interaction.state};
  }
};

enum class Axis : uint8_t {
  Row,
  Column,
};

enum class Align : uint8_t {
  Start,
  Center,
  End,
  Stretch,
};

struct Slot {
  int16_t basis = 0;
  uint8_t flex = 0;
  Rect rect{};
};

template <size_t MaxSlots>
class Stack {
 public:
  Stack(Rect rect, Axis axis, int16_t gap = 0) : rect_(rect), axis_(axis), gap_(gap) {}

  int8_t fixed(int16_t px) { return add(Slot{px, 0, {}}); }
  int8_t flex(uint8_t grow = 1, int16_t minPx = 0) { return add(Slot{minPx, grow, {}}); }
  int8_t autoSize(Size size) { return fixed(axis_ == Axis::Row ? size.width : size.height); }

  void layout() {
    int16_t fixedTotal = 0;
    uint16_t flexTotal = 0;
    int8_t lastFlex = -1;
    for (uint8_t i = 0; i < count_; ++i) {
      fixedTotal = static_cast<int16_t>(fixedTotal + slots_[i].basis);
      flexTotal = static_cast<uint16_t>(flexTotal + slots_[i].flex);
      if (slots_[i].flex) lastFlex = static_cast<int8_t>(i);
    }
    const int16_t totalGap = count_ > 1 ? static_cast<int16_t>((count_ - 1) * gap_) : 0;
    const int16_t mainSize = axis_ == Axis::Row ? rect_.width : rect_.height;
    int16_t remaining = static_cast<int16_t>(mainSize - fixedTotal - totalGap);
    if (remaining < 0) remaining = 0;

    int16_t cursor = axis_ == Axis::Row ? rect_.x : rect_.y;
    int16_t allocatedFlex = 0;
    for (uint8_t i = 0; i < count_; ++i) {
      int16_t main = slots_[i].basis;
      if (slots_[i].flex && flexTotal) {
        // The last flex slot absorbs the integer-division remainder so flex
        // layouts always fill the rect exactly, even with a fixed slot after.
        int16_t share = static_cast<int16_t>((static_cast<int32_t>(remaining) * slots_[i].flex) / flexTotal);
        if (static_cast<int8_t>(i) == lastFlex) share = static_cast<int16_t>(remaining - allocatedFlex);
        allocatedFlex = static_cast<int16_t>(allocatedFlex + share);
        main = static_cast<int16_t>(main + share);
      }
      if (axis_ == Axis::Row) {
        slots_[i].rect = Rect{cursor, rect_.y, main, rect_.height};
      } else {
        slots_[i].rect = Rect{rect_.x, cursor, rect_.width, main};
      }
      cursor = static_cast<int16_t>(cursor + main + gap_);
    }
  }

  uint8_t count() const { return count_; }
  Rect rect(uint8_t index) const { return index < count_ ? slots_[index].rect : Rect{}; }

 private:
  Rect rect_{};
  Axis axis_ = Axis::Column;
  int16_t gap_ = 0;
  Slot slots_[MaxSlots]{};
  uint8_t count_ = 0;

  int8_t add(Slot slot) {
    if (count_ >= MaxSlots) return -1;
    slots_[count_] = slot;
    return static_cast<int8_t>(count_++);
  }
};

template <size_t MaxInteractions>
class Frame {
 public:
  Frame(DrawTarget& target, const DeviceContext& device, const InputSnapshot& input,
        InteractionBuffer<MaxInteractions>& interactions, AssetResolver* assets = nullptr)
      : target_(target), device_(device), input_(input), interactions_(interactions), assets_(assets) {
    interactions_.clear();
  }

  DrawTarget& target() { return target_; }
  AssetResolver* assets() { return assets_; }
  const DeviceContext& device() const { return device_; }
  Rect screen() const { return device_.screen(); }
  Rect safeRect() const { return device_.safeRect(); }

  bool hit(Rect rect, ActionId action, int16_t value = 0, uint16_t inputMask = InputDefault,
           State state = StateNormal) {
    return interactions_.addInteraction(Interaction{rect, action, value, inputMask, state, 0});
  }

  State stateFor(ActionId action, int16_t value = 0, State base = StateNormal) const {
    return interactions_.stateFor(action, value, base);
  }

  ActionEvent finish() { return interactions_.route(input_); }

 private:
  DrawTarget& target_;
  const DeviceContext& device_;
  const InputSnapshot& input_;
  InteractionBuffer<MaxInteractions>& interactions_;
  AssetResolver* assets_ = nullptr;
};

struct ButtonProps {
  const char* label = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  ActionId action = NO_ACTION;
  int16_t value = 0;
  uint16_t inputMask = InputDefault;
  State state = StateNormal;
  TextStyle text{};
  StyleSet styles{};
  int16_t minTouchSize = 44;
  uint8_t radius = 0;
  // Extra hit area beyond the visual rect, per edge. Use this to give
  // adjacent controls contiguous, non-overlapping tap bands (split the gap
  // between a stepper's -/+ instead of letting centered minTouchSize
  // expansion overlap the neighbor). Composes with minTouchSize, screen
  // clamping, and edge snapping; the visual rect is unchanged.
  Insets hitPadding{};
  int16_t gap = 4;
  uint8_t borderEdges = EdgesAll;
  bool enabled = true;
};

struct HeaderProps {
  const char* title = nullptr;
  const char* subtitle = nullptr;
  const char* rightLabel = nullptr;
  TextStyle titleText{};
  TextStyle subtitleText{};
  StyleSet styles{};
  int16_t titleOffsetY = 0;
  uint8_t borderEdges = EdgesAll;
  bool centered = false;
};

struct ListItem {
  const char* label = nullptr;
  const char* subtitle = nullptr;
  const char* value = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  State state = StateNormal;
  int16_t actionValue = 0;
  bool enabled = true;
  // Section header row: shorter, non-interactive, drawn with headerText and
  // an underline; never selected or focused.
  bool isHeader = false;
};

enum class SelectionMarker : uint8_t {
  None,       // selection shown by the row's selected BoxStyle
  Underline,  // thin line under the selected row's content
  Triangle,   // right-pointing triangle at the selected row's left edge
};

struct ListProps {
  const ListItem* items = nullptr;
  uint16_t count = 0;
  // First item index drawn at the top of the rect. The list is virtualized:
  // only the rows that fully fit inside the rect are laid out, drawn, and
  // registered for interaction. Use listVisibleRows()/listTopIndexFor() to
  // keep a selection in view while scrolling.
  uint16_t topIndex = 0;
  int16_t selectedIndex = -1;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle labelText{};
  TextStyle subtitleText{};
  TextStyle valueText{};
  StyleSet rowStyles{};
  int16_t rowHeight = 36;
  int16_t rowGap = 0;
  uint8_t rowRadius = 0;
  int16_t sidePadding = 8;
  int16_t textGap = 6;
  int16_t iconSize = 0;
  int16_t scrollIndicatorWidth = 3;
  bool centerSingleLine = false;
  // Shrink each row's background/hit area to its label width plus side
  // padding instead of the full rect width (hug-content menu rows).
  bool hugContents = false;
  // Draws a thin position indicator along the right edge when the list
  // overflows the rect.
  bool scrollIndicator = true;
  // Additional marker drawn on the selected row (the v1 theme Underline and
  // Triangle selection styles).
  SelectionMarker selectionMarker = SelectionMarker::None;
  Paint markerPaint = Paint::solid(Color::Black);
  int16_t markerInset = 0;       // x offset of the triangle / underline start
  int16_t markerThickness = 2;   // underline thickness
  // Section header rows (ListItem::isHeader).
  TextStyle headerText{};
  int16_t headerRowHeight = 0;  // 0 = headerText line height + underline gap
  int16_t sectionGap = 16;      // extra padding above a non-first header
  bool headerUnderline = true;
};

struct ProgressBarProps {
  int32_t value = 0;
  int32_t max = 100;
  Paint track = Paint::none();
  Paint fill = Paint::solid(Color::Black);
  Paint border = Paint::none();
  uint8_t borderWidth = 0;
  uint8_t radius = 0;
  // Minimum fill width for any nonzero value, so chart-style bars (reading
  // stats time-of-day/day-of-week) stay visible when a value rounds to 0px.
  int16_t minFill = 0;
};

struct SliderProps {
  int32_t value = 0;
  int32_t max = 100;
  ActionId action = NO_ACTION;
  int16_t actionValue = 0;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  Paint track = Paint::dither(Color::LightGray);
  Paint fill = Paint::solid(Color::Black);
  Paint knob = Paint::solid(Color::Black);
  Paint border = Paint::solid(Color::Black);
  int16_t trackHeight = 4;
  int16_t knobWidth = 14;
  int16_t knobHeight = 22;
  int16_t horizontalPadding = 8;
  int16_t minTouchSize = 44;
  uint8_t radius = 0;
  bool enabled = true;
};

struct CheckboxProps {
  const char* label = nullptr;
  bool checked = false;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  uint16_t inputMask = InputDefault;
  TextStyle text{};
  StyleSet styles{};
  State state = StateNormal;
  int16_t boxSize = 18;
  int16_t sidePadding = 8;
  int16_t gap = 8;
  int16_t minTouchSize = 44;
  uint8_t radius = 0;
  bool enabled = true;
};

struct StatusBarProps {
  const char* title = nullptr;
  const char* leading = nullptr;
  // Second left-cluster item drawn after `leading` (e.g. estimated time left
  // next to the clock/battery in a reader status bar).
  const char* leadingSecondary = nullptr;
  const char* trailing = nullptr;
  // Second right-cluster item drawn left of `trailing` (e.g. a clock next to
  // the page-progress text).
  const char* trailingSecondary = nullptr;
  // Width at the left edge occupied by app-drawn chrome (e.g. a theme's own
  // battery glyph); the clusters and title stay clear of it.
  int16_t leadingReserve = 0;
  // Shifts the title upward (legacy reader chrome fine-tuning).
  int16_t titleOffsetY = 0;
  BitmapRef leadingIcon{};
  AssetRef leadingIconAsset{};
  ProgressBarProps progress{};
  TextStyle text{};
  int16_t horizontalPadding = 6;
  int16_t gap = 8;
  int16_t progressHeight = 4;
  bool showProgress = false;
  bool fillBackground = false;
  Paint background = Paint::solid(Color::White);
};

struct PopupProps {
  const char* message = nullptr;
  TextStyle text{};
  StyleSet styles{};
  Insets padding{12, 16, 12, 16};
  int16_t maxWidth = 0;  // 0 = 3/4 bounds width when auto-sized by Screen::popup.
  ProgressBarProps progress{};
  int16_t progressHeight = 4;
  bool showProgress = false;
};

struct TextFieldProps {
  const char* text = nullptr;
  TextStyle textStyle{};
  StyleSet styles{};
  uint16_t cursor = 0;
  bool cursorVisible = false;
  bool cursorBlock = false;
  bool selected = false;
  bool disabled = false;
  int16_t horizontalPadding = 6;
};

struct DropdownProps {
  const char* label = nullptr;
  const char* value = nullptr;
  ActionId action = NO_ACTION;
  int16_t actionValue = 0;
  uint16_t inputMask = InputDefault;
  TextStyle labelText{};
  TextStyle valueText{};
  StyleSet styles{};
  State state = StateNormal;
  Insets padding{6, 8, 6, 8};
  int16_t gap = 8;
  int16_t indicatorWidth = 12;
  int16_t indicatorSize = 8;
  uint8_t indicatorStroke = 1;
  int16_t minTouchSize = 44;
  uint8_t radius = 0;
  bool enabled = true;
};

struct TableProps {
  const char* const* cells = nullptr;
  uint8_t rows = 0;
  uint8_t cols = 0;
  TextStyle text{};
  StyleSet cellStyles{};
  Paint grid = Paint::solid(Color::Black);
  uint8_t gridWidth = 1;
  uint8_t cellRadius = 0;
  int16_t rowHeight = 24;
  int16_t padding = 4;
  bool headerRow = false;
};

enum class KeyKind : uint8_t {
  Normal,
  Shift,
  Mode,
  Space,
  Delete,
  Ok,
  Disabled,
};

struct KeyGridKey {
  const char* label = nullptr;
  const char* secondaryLabel = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  KeyKind kind = KeyKind::Normal;
  State state = StateNormal;
  int16_t value = 0;
  bool enabled = true;
};

struct KeyGridProps {
  const KeyGridKey* keys = nullptr;
  uint8_t rows = 0;
  uint8_t cols = 0;
  int16_t selectedIndex = -1;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault;
  TextStyle labelText{};
  TextStyle secondaryText{};
  StyleSet keyStyles{};
  int16_t gap = 0;
  int16_t minTouchSize = 28;
  uint8_t radius = 0;
  bool inactiveSelection = false;
};

constexpr int16_t QWERTY_KEY_SHIFT = -1;
constexpr int16_t QWERTY_KEY_MODE = -2;
constexpr int16_t QWERTY_KEY_BACKSPACE = 8;
constexpr int16_t QWERTY_KEY_ENTER = 13;
constexpr int16_t QWERTY_KEY_SPACE = 32;

enum class KeyboardLayoutId : uint8_t {
  QwertyEn,
  AzertyFr,
  QwertzDe,
  SpanishEs,
};

struct KeyboardKey {
  const char* label = nullptr;   // UTF-8 visual label.
  const char* output = nullptr;  // UTF-8 text the app may insert for normal keys.
  KeyKind kind = KeyKind::Normal;
  State state = StateNormal;
  int16_t value = 0;       // Stable key id returned in ActionEvent::value.
  uint8_t widthUnits = 1;  // Relative visual width within the row.
  bool enabled = true;
};

struct KeyboardRow {
  const KeyboardKey* keys = nullptr;
  uint8_t count = 0;
  uint8_t insetUnits = 0;
};

struct KeyboardLayout {
  const KeyboardRow* rows = nullptr;
  uint8_t rowCount = 0;
};

struct KeyboardProps {
  const KeyboardLayout* layout = nullptr;
  ActionId keyAction = NO_ACTION;
  ActionId shiftAction = NO_ACTION;
  ActionId modeAction = NO_ACTION;
  ActionId deleteAction = NO_ACTION;
  ActionId okAction = NO_ACTION;
  uint16_t inputMask = InputDefault;
  int16_t selectedIndex = -1;
  TextStyle labelText{};
  StyleSet keyStyles{};
  Insets padding{5, 5, 5, 5};
  int16_t gap = 3;
  int16_t minTouchSize = 28;
  uint8_t keyRadius = 0;
  bool inactiveSelection = false;
};

struct QwertyKeyboardProps {
  ActionId keyAction = NO_ACTION;
  ActionId shiftAction = NO_ACTION;
  ActionId modeAction = NO_ACTION;
  ActionId deleteAction = NO_ACTION;
  ActionId okAction = NO_ACTION;
  uint16_t inputMask = InputDefault;
  int16_t selectedIndex = -1;
  TextStyle labelText{};
  StyleSet keyStyles{};
  Insets padding{5, 5, 5, 5};
  int16_t gap = 3;
  int16_t minTouchSize = 28;
  uint8_t keyRadius = 0;
  KeyboardLayoutId layout = KeyboardLayoutId::QwertyEn;
  bool shifted = false;
  bool symbols = false;
  bool inactiveSelection = false;
};

struct GestureBarButton {
  const char* label = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  ActionId action = NO_ACTION;
  int16_t value = 0;
  State state = StateNormal;
};

struct GestureBarProps {
  GestureBarButton left{};
  GestureBarButton center{};
  GestureBarButton right{};
  ActionId swipeLeft = NO_ACTION;
  ActionId swipeRight = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  int16_t height = 44;
  int16_t gap = 0;
  TextStyle text{};
  StyleSet styles{};
};

struct TabItem {
  const char* label = nullptr;
  int16_t value = 0;
  bool selected = false;
};

struct TabBarProps {
  const TabItem* tabs = nullptr;
  uint8_t count = 0;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle text{};
  // selected state drives the pill: set selected.background + radius.
  StyleSet tabStyles{};
  // Inset of each tab pill within its equal-width slot.
  Insets tabInset{4, 4, 8, 4};
  // 1px divider along the bottom edge (RoundedRaff-style settings tabs).
  bool divider = false;
  Paint dividerPaint = Paint::solid(Color::Black);
};

// One laid-out carousel slot. The component owns geometry, selection chrome,
// and input routing; the app draws the cover art into `content` (image
// decoding stays app-owned, and deterministic rects keep app-side frame
// caching valid).
struct CarouselSlot {
  bool valid = false;
  bool isCenter = false;
  int16_t itemIndex = -1;
  Rect frame{};    // outer frame (selection chrome drawn here)
  Rect content{};  // inside the frame inset; render the cover art here
};

struct CarouselProps {
  uint16_t count = 0;
  int16_t selectedIndex = 0;
  // Tapping any cover fires this action with the item index as value;
  // prev/next buttons and swipes re-fire it with the neighbor indexes.
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault;
  Size centerSize{220, 320};
  Size sideSize{120, 180};
  int16_t gap = 12;
  int16_t contentInset = 4;
  // Wrap to the other end at the edges; otherwise edge slots are invalid.
  bool wrap = false;
  // normal styles the side frames, selected styles the center frame.
  StyleSet frameStyles{};
};

struct BatteryIndicatorProps {
  uint8_t percent = 0;  // 0..100
  bool charging = false;
  // Optional pre-formatted label (e.g. "82%"), drawn left of the glyph. The
  // component never formats text itself; the app owns strings.
  const char* label = nullptr;
  TextStyle text{};
  Color color = Color::Black;
  // Glyph size; the glyph is right-aligned and vertically centered in the
  // rect. Width excludes the terminal nub.
  int16_t glyphWidth = 22;
  int16_t glyphHeight = 11;
  int16_t gap = 4;
  // Optional bolt/plug icon drawn over the glyph while charging. Without it,
  // charging is shown by a dithered fill instead of a solid one.
  BitmapRef chargingIcon{};
};

struct MetricCardProps {
  const char* label = nullptr;    // small text at the top
  const char* value = nullptr;    // large text in the middle
  const char* unit = nullptr;     // small text appended after the value
  const char* caption = nullptr;  // small text at the bottom
  TextStyle labelText{};
  TextStyle valueText{};
  TextStyle captionText{};
  StyleSet styles{};
  Insets padding{6, 8, 6, 8};
  ActionId action = NO_ACTION;  // optional: makes the whole card tappable
  int16_t actionValue = 0;
  uint16_t inputMask = InputDefault;
  State state = StateNormal;
  int16_t gap = 2;
  bool centered = true;
};

struct DialogOption {
  const char* label = nullptr;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  State state = StateNormal;
  bool enabled = true;
};

struct OptionDialogProps {
  // Three optional text slots, top to bottom: a small caption ("Skip this
  // event?"), a prominent multi-line headline (an event or book title), and a
  // body line (a time range, a detail). Each renders with its own style and
  // honors that style's alignment and maxLines.
  const char* title = nullptr;
  const char* headline = nullptr;
  const char* message = nullptr;
  const DialogOption* options = nullptr;
  uint8_t optionCount = 0;
  TextStyle titleText{};
  TextStyle headlineText{};
  TextStyle messageText{};
  TextStyle buttonText{};
  StyleSet styles{};        // dialog panel
  StyleSet buttonStyles{};  // option buttons
  Insets padding{12, 16, 12, 16};
  int16_t buttonHeight = 44;
  int16_t gap = 8;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  // Stack options vertically (one per row) instead of in one bottom row;
  // better for narrow portrait panels or more than three options.
  bool verticalOptions = false;
  // Dim everything behind the dialog with a dither so stale chrome reads as
  // inactive on e-paper.
  bool dimBackground = false;
};

struct TapZone {
  Rect rect{};
  ActionId action = NO_ACTION;
  int16_t value = 0;
  uint16_t inputMask = InputTouch;
  State state = StateNormal;
  bool enabled = true;
};

struct TapZonesProps {
  const TapZone* zones = nullptr;
  uint8_t count = 0;
  ActionId swipeLeft = NO_ACTION;
  ActionId swipeRight = NO_ACTION;
  ActionId back = NO_ACTION;
};

struct ReaderChromeProps {
  StatusBarProps top{};
  StatusBarProps bottom{};
  int16_t topHeight = 28;
  int16_t bottomHeight = 32;
  bool showTop = true;
  bool showBottom = true;
  bool dimContent = false;
};

struct SettingRowProps {
  const char* label = nullptr;
  const char* subtitle = nullptr;
  const char* value = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  ActionId action = NO_ACTION;
  int16_t valueId = 0;
  uint16_t inputMask = InputDefault;
  TextStyle labelText{};
  TextStyle subtitleText{};
  TextStyle valueText{};
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;
  uint8_t radius = 0;
  int16_t sidePadding = 8;
  int16_t textGap = 6;
  int16_t iconSize = 0;
  int16_t minTouchSize = 44;
  bool drawChevron = false;
};

struct ToggleRowProps {
  SettingRowProps row{};
  bool checked = false;
  ActionId toggleAction = NO_ACTION;
  int16_t toggleValue = 0;
  int16_t toggleWidth = 38;
  int16_t toggleHeight = 18;
  uint8_t radius = 0;
  uint8_t knobRadius = 0;
  int16_t knobInset = 3;
  uint8_t borderWidth = 1;
  Paint track = Paint::solid(Color::White);
  Paint checkedTrack = Paint::solid(Color::Black);
  Paint border = Paint::solid(Color::Black);
  Paint knob = Paint::solid(Color::Black);
  Paint checkedKnob = Paint::solid(Color::White);
};

struct StepperRowProps {
  SettingRowProps row{};
  const char* value = nullptr;
  ActionId decrement = NO_ACTION;
  ActionId increment = NO_ACTION;
  int16_t decrementValue = -1;
  int16_t incrementValue = 1;
  int16_t buttonWidth = 32;
  int16_t buttonHeight = 28;
  int16_t valueWidth = 44;
  int16_t gap = 6;
  StyleSet buttonStyles{};
  Paint controlPaint = Paint::solid(Color::Black);
  int16_t controlSize = 8;
  uint8_t controlStroke = 2;
  uint8_t buttonRadius = 0;
};

struct RadioOption {
  const char* label = nullptr;
  int16_t value = 0;
  bool enabled = true;
};

struct RadioGroupProps {
  const RadioOption* options = nullptr;
  uint8_t count = 0;
  int16_t selectedValue = 0;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle text{};
  StyleSet styles{};
  int16_t gap = 4;
  int16_t minTouchSize = 44;
  uint8_t radius = 0;
};

struct ContextMenuProps {
  const char* title = nullptr;
  const DialogOption* options = nullptr;
  uint8_t optionCount = 0;
  TextStyle titleText{};
  TextStyle itemText{};
  StyleSet panelStyles{};
  StyleSet itemStyles{};
  Insets padding{10, 10, 10, 10};
  int16_t rowHeight = 40;
  int16_t gap = 2;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  bool dimBackground = true;
};

enum class ToastAnchor : uint8_t {
  Top,
  Center,
  Bottom,
};

struct ToastProps {
  const char* message = nullptr;
  TextStyle text{};
  StyleSet styles{};
  Insets padding{8, 12, 8, 12};
  ToastAnchor anchor = ToastAnchor::Bottom;
  int16_t maxWidth = 0;  // 0 = 3/4 screen width
  int16_t margin = 16;
};

struct MessagePanelProps {
  const char* title = nullptr;
  const char* message = nullptr;
  const char* actionLabel = nullptr;
  ActionId action = NO_ACTION;
  int16_t actionValue = 0;
  TextStyle titleText{};
  TextStyle messageText{};
  TextStyle buttonText{};
  StyleSet panelStyles{};
  StyleSet buttonStyles{};
  Insets padding{16, 20, 16, 20};
  int16_t gap = 8;
  int16_t buttonHeight = 40;
  bool showProgress = false;
  ProgressBarProps progress{};
};

struct BookCardProps {
  const char* title = nullptr;
  const char* author = nullptr;
  const char* meta = nullptr;
  BitmapRef cover{};
  AssetRef coverAsset{};
  int32_t progress = 0;
  int32_t progressMax = 100;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  TextStyle titleText{};
  TextStyle authorText{};
  TextStyle metaText{};
  StyleSet styles{};
  State state = StateNormal;
  bool enabled = true;
  Size coverSize{62, 84};
  Insets padding{6, 8, 6, 8};
  int16_t gap = 14;
  int16_t textGap = 4;
  int16_t textProgressGap = 8;
  int16_t progressHeight = 4;
};

struct CoverGridItem {
  const char* title = nullptr;
  BitmapRef cover{};
  AssetRef coverAsset{};
  State state = StateNormal;
  int16_t actionValue = 0;
  bool enabled = true;
};

struct CoverGridProps {
  const CoverGridItem* items = nullptr;
  uint16_t count = 0;
  uint16_t topIndex = 0;
  int16_t selectedIndex = -1;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle titleText{};
  StyleSet cellStyles{};
  uint8_t columns = 3;
  Size coverSize{72, 104};
  int16_t rowHeight = 132;
  int16_t gap = 8;
  int16_t labelHeight = 20;
  int16_t minTouchSize = 44;
};

// Panel height optionDialog needs at the given width — thin sugar over
// measureWrappedText so callers size the dialog instead of guessing:
//
//   const int16_t h = optionDialogHeight(ui.target(), props, 340);
//   optionDialog(ui, centeredRect(ui.safeRect(), {340, h}), props);
inline int16_t optionDialogHeight(const DrawTarget& target, const OptionDialogProps& props, const int16_t width) {
  const int16_t contentW = static_cast<int16_t>(width - props.padding.left - props.padding.right);
  int16_t height = static_cast<int16_t>(props.padding.top + props.padding.bottom);
  if (props.title) {
    height = static_cast<int16_t>(height + target.lineHeight(props.titleText.font) + props.gap);
  }
  if (props.headline) {
    height = static_cast<int16_t>(height + measureWrappedText(target, props.headline, props.headlineText, contentW).height +
                                  props.gap);
  }
  if (props.message) {
    height = static_cast<int16_t>(height + measureWrappedText(target, props.message, props.messageText, contentW).height);
  }
  if (props.options && props.optionCount > 0) {
    const int16_t buttonsH =
        props.verticalOptions
            ? static_cast<int16_t>(props.optionCount * props.buttonHeight + (props.optionCount - 1) * props.gap)
            : props.buttonHeight;
    height = static_cast<int16_t>(height + props.gap + buttonsH);
  }
  return height;
}

StyleSet defaultButtonStyles();
StyleSet defaultListRowStyles();
StyleSet defaultKeyStyles();
StyleSet defaultPopupStyles();
ThemeTokens defaultThemeTokens(FontId smallFont = 0, FontId bodyFont = 0, FontId titleFont = 0);
Rect centeredRect(Rect outer, Size inner);
Rect ensureMinTouchRect(Rect visual, int16_t minSize, Rect bounds);
// Number of rows that fully fit in the rect at the given row height/gap.
uint16_t listVisibleRows(Rect rect, int16_t rowHeight, int16_t rowGap = 0);
// Adjusts topIndex the minimal amount so selectedIndex is visible, then clamps
// to the valid scroll range. Pass the current topIndex to keep the window
// stable when the selection is already on-screen.
uint16_t listTopIndexFor(int16_t selectedIndex, uint16_t topIndex, uint16_t visibleRows, uint16_t count);
// Shared immediate-mode list navigation helpers for apps that render lists
// with their own theme/renderer but want SDK-owned selection semantics.
inline int listClampedIndex(const int index, const int count) {
  if (count <= 0) return 0;
  if (index < 0) return 0;
  if (index >= count) return count - 1;
  return index;
}
inline bool listSelectIndex(int& selectedIndex, const int requestedIndex, const int count) {
  if (count <= 0 || requestedIndex < 0 || requestedIndex >= count || selectedIndex == requestedIndex) return false;
  selectedIndex = requestedIndex;
  return true;
}
inline bool listSelectIndex(size_t& selectedIndex, const int requestedIndex, const int count) {
  if (count <= 0 || requestedIndex < 0 || requestedIndex >= count ||
      selectedIndex == static_cast<size_t>(requestedIndex))
    return false;
  selectedIndex = static_cast<size_t>(requestedIndex);
  return true;
}
inline bool listMoveIndex(int& selectedIndex, const int delta, const int count) {
  if (count <= 0 || delta == 0) return false;
  const int oldIndex = selectedIndex;
  selectedIndex = (selectedIndex + delta + count) % count;
  return selectedIndex != oldIndex;
}
inline bool listMoveIndex(size_t& selectedIndex, const int delta, const int count) {
  if (count <= 0 || delta == 0) return false;
  int selected = static_cast<int>(selectedIndex);
  const bool changed = listMoveIndex(selected, delta, count);
  selectedIndex = static_cast<size_t>(selected);
  return changed;
}
inline bool listPageIndex(int& selectedIndex, const int deltaPages, const int count, int pageItems) {
  if (count <= 0 || deltaPages == 0) return false;
  if (pageItems < 1) pageItems = 1;
  const int oldIndex = selectedIndex;
  selectedIndex = listClampedIndex(selectedIndex + deltaPages * pageItems, count);
  return selectedIndex != oldIndex;
}
void drawText(DrawTarget& target, Rect rect, const char* text, TextStyle style);
void drawBitmap(DrawTarget& target, Rect rect, BitmapRef bitmap, BitmapMode mode,
                Paint foreground = Paint::solid(Color::Black));
BitmapRef resolveBitmap(AssetResolver* resolver, const AssetRef& asset);
int16_t clampInt16(int32_t value, int16_t minValue, int16_t maxValue);
TextStyle textStyleWithForeground(TextStyle text, Paint foreground);
const KeyboardLayout& builtinKeyboardLayout(KeyboardLayoutId id, bool shifted = false, bool symbols = false);

inline void setStyleRadius(StyleSet& styles, uint8_t radius) {
  styles.normal.radius = radius;
  styles.selected.radius = radius;
  styles.focused.radius = radius;
  styles.active.radius = radius;
  styles.disabled.radius = radius;
}

inline BitmapRef lucideDeleteIcon16() {
  static constexpr uint8_t bits[] = {
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8, 0x01, 0xF3, 0xFD, 0xE7,
      0xFD, 0xCF, 0x6D, 0xBF, 0x9D, 0xBF, 0x9D, 0xCF, 0x6D, 0xE7, 0xFD,
      0xF3, 0xFD, 0xF8, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  };
  return BitmapRef{bits, 16, 16, BitmapFormat::Mask1};
}

inline void drawBorderEdges(DrawTarget& target, Rect rect, Paint paint, uint8_t width, uint8_t radius, uint8_t corners,
                            uint8_t edges) {
  if (paint.kind == PaintKind::None || width == 0 || edges == EdgesNone || rect.empty()) return;
  if ((edges & EdgesAll) == EdgesAll) {
    target.stroke(rect, paint, width, radius, corners);
    return;
  }
  if (edges & EdgeTop) {
    target.line(Point{rect.x, rect.y}, Point{static_cast<int16_t>(rect.right() - 1), rect.y}, width, paint);
  }
  if (edges & EdgeRight) {
    const int16_t x = static_cast<int16_t>(rect.right() - 1);
    target.line(Point{x, rect.y}, Point{x, static_cast<int16_t>(rect.bottom() - 1)}, width, paint);
  }
  if (edges & EdgeBottom) {
    const int16_t y = static_cast<int16_t>(rect.bottom() - 1);
    target.line(Point{rect.x, y}, Point{static_cast<int16_t>(rect.right() - 1), y}, width, paint);
  }
  if (edges & EdgeLeft) {
    target.line(Point{rect.x, rect.y}, Point{rect.x, static_cast<int16_t>(rect.bottom() - 1)}, width, paint);
  }
}

template <size_t MaxInteractions>
void button(Frame<MaxInteractions>& frame, Rect rect, const ButtonProps& props) {
  if (props.enabled && props.action != NO_ACTION) {
    const Rect padded{static_cast<int16_t>(rect.x - props.hitPadding.left),
                      static_cast<int16_t>(rect.y - props.hitPadding.top),
                      static_cast<int16_t>(rect.width + props.hitPadding.left + props.hitPadding.right),
                      static_cast<int16_t>(rect.height + props.hitPadding.top + props.hitPadding.bottom)};
    Rect hitRect = ensureMinTouchRect(padded, props.minTouchSize, frame.screen());
    frame.hit(hitRect, props.action, props.value, props.inputMask, props.state);
  }

  StyleSet styles = props.styles.unset() ? defaultButtonStyles() : props.styles;
  if (props.radius > 0) setStyleRadius(styles, props.radius);
  State state = props.enabled ? props.state : static_cast<State>(props.state | StateDisabled);
  state = frame.stateFor(props.action, props.value, state);
  const BoxStyle& style = styles.resolve(state);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    drawBorderEdges(frame.target(), rect, style.border, style.borderWidth, style.radius, style.corners,
                    props.borderEdges);
  }

  Rect content = rect.inset(Insets{2, 4, 2, 4});
  BitmapRef icon = props.icon ? props.icon : resolveBitmap(frame.assets(), props.iconAsset);
  if (icon && props.label) {
    Size labelSize = frame.target().measureText(props.text.font, props.label, props.text);
    int16_t totalW = static_cast<int16_t>(icon.width + props.gap + labelSize.width);
    int16_t x = static_cast<int16_t>(content.x + (content.width - totalW) / 2);
    Rect iconRect{x, static_cast<int16_t>(content.y + (content.height - icon.height) / 2),
                  static_cast<int16_t>(icon.width), static_cast<int16_t>(icon.height)};
    frame.target().bitmap(iconRect, icon, BitmapMode::Center, style.foreground);
    Rect textRect{static_cast<int16_t>(x + icon.width + props.gap), content.y,
                  static_cast<int16_t>(content.right() - x - icon.width - props.gap), content.height};
    TextStyle textStyle = textStyleWithForeground(props.text, style.foreground);
    textStyle.align = TextAlign::Left;
    frame.target().text(textRect, props.label, textStyle);
  } else if (icon) {
    Rect iconRect = centeredRect(content, Size{static_cast<int16_t>(icon.width), static_cast<int16_t>(icon.height)});
    frame.target().bitmap(iconRect, icon, BitmapMode::Center, style.foreground);
  } else if (props.label) {
    frame.target().text(content, props.label, textStyleWithForeground(props.text, style.foreground));
  }
}

template <size_t MaxInteractions>
void header(Frame<MaxInteractions>& frame, Rect rect, const HeaderProps& props) {
  StyleSet styles = props.styles.unset() ? defaultPopupStyles() : props.styles;
  const BoxStyle& style = styles.resolve(StateNormal);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    drawBorderEdges(frame.target(), rect, style.border, style.borderWidth, style.radius, style.corners,
                    props.borderEdges);
  }

  Rect content = rect.inset(Insets{0, 6, 0, 6});
  TextStyle titleStyle = props.titleText;
  titleStyle.align = props.centered ? TextAlign::Center : TextAlign::Left;
  if (props.title) {
    Rect titleRect{content.x, static_cast<int16_t>(content.y + props.titleOffsetY), content.width, content.height};
    if (props.rightLabel) {
      const Size rightSize = frame.target().measureText(props.subtitleText.font, props.rightLabel, props.subtitleText);
      Rect rightRect{static_cast<int16_t>(content.right() - rightSize.width), content.y, rightSize.width,
                     content.height};
      frame.target().text(rightRect, props.rightLabel, props.subtitleText);
      titleRect.width = static_cast<int16_t>(titleRect.width - rightSize.width - 6);
    }
    frame.target().text(titleRect, props.title, titleStyle);
  }
  if (props.subtitle) {
    Rect subRect{content.x, static_cast<int16_t>(content.y + frame.target().lineHeight(props.titleText.font)),
                 content.width, static_cast<int16_t>(content.height - frame.target().lineHeight(props.titleText.font))};
    frame.target().text(subRect, props.subtitle, props.subtitleText);
  }
}

template <size_t MaxInteractions>
void progressBar(Frame<MaxInteractions>& frame, Rect rect, const ProgressBarProps& props) {
  frame.target().fill(rect, props.track);
  if (props.border.kind != PaintKind::None && props.borderWidth > 0) {
    frame.target().stroke(rect, props.border, props.borderWidth, props.radius);
  }
  if (props.max <= 0 || props.value <= 0) return;
  const int32_t clamped = props.value > props.max ? props.max : props.value;
  int16_t fillWidth = static_cast<int16_t>((static_cast<int32_t>(rect.width) * clamped) / props.max);
  if (fillWidth < props.minFill) fillWidth = props.minFill > rect.width ? rect.width : props.minFill;
  if (fillWidth > 0) {
    frame.target().fill(Rect{rect.x, rect.y, fillWidth, rect.height}, props.fill);
  }
}

template <size_t MaxInteractions>
void slider(Frame<MaxInteractions>& frame, Rect rect, const SliderProps& props) {
  State state = props.enabled ? StateNormal : StateDisabled;
  if (props.enabled && props.action != NO_ACTION) {
    frame.hit(ensureMinTouchRect(rect, props.minTouchSize, frame.screen()), props.action, props.actionValue,
              props.inputMask, state);
  }
  state = frame.stateFor(props.action, props.actionValue, state);

  Rect content = rect.inset(Insets{0, props.horizontalPadding, 0, props.horizontalPadding});
  if (content.empty()) return;
  const int16_t trackH = props.trackHeight < 1 ? 1 : props.trackHeight;
  Rect track{content.x, static_cast<int16_t>(content.y + (content.height - trackH) / 2), content.width, trackH};
  ProgressBarProps bar;
  bar.value = props.value;
  bar.max = props.max;
  bar.track = props.track;
  bar.fill = hasState(state, StateDisabled) ? Paint::dither(Color::LightGray) : props.fill;
  bar.radius = props.radius;
  progressBar(frame, track, bar);

  const int32_t max = props.max <= 0 ? 1 : props.max;
  int32_t value = props.value < 0 ? 0 : props.value;
  if (value > max) value = max;
  const int16_t knobW = props.knobWidth < 4 ? 4 : props.knobWidth;
  const int16_t knobH = props.knobHeight < trackH ? trackH : props.knobHeight;
  const int16_t travel = content.width > knobW ? static_cast<int16_t>(content.width - knobW) : 0;
  const int16_t knobX = static_cast<int16_t>(content.x + (static_cast<int32_t>(travel) * value) / max);
  Rect knobRect{knobX, static_cast<int16_t>(content.y + (content.height - knobH) / 2), knobW, knobH};
  frame.target().fill(knobRect, hasState(state, StateDisabled) ? Paint::dither(Color::LightGray) : props.knob,
                      props.radius);
  if (props.border.kind != PaintKind::None) frame.target().stroke(knobRect, props.border, 1, props.radius);
}

template <size_t MaxInteractions>
void checkbox(Frame<MaxInteractions>& frame, Rect rect, const CheckboxProps& props) {
  State state = props.enabled ? props.state : static_cast<State>(props.state | StateDisabled);
  if (props.checked) state |= StateChecked;
  if (props.enabled && props.action != NO_ACTION) {
    frame.hit(ensureMinTouchRect(rect, props.minTouchSize, frame.screen()), props.action, props.value,
              props.inputMask, state);
  }
  state = frame.stateFor(props.action, props.value, state);
  StyleSet styles = props.styles.unset() ? defaultButtonStyles() : props.styles;
  if (props.radius > 0) setStyleRadius(styles, props.radius);
  const BoxStyle& style = styles.resolve(state);

  const int16_t box = props.boxSize < 8 ? 8 : props.boxSize;
  Rect boxRect{static_cast<int16_t>(rect.x + props.sidePadding),
               static_cast<int16_t>(rect.y + (rect.height - box) / 2), box, box};
  frame.target().fill(boxRect, style.background, style.radius, style.corners);
  frame.target().stroke(boxRect, style.border.kind == PaintKind::None ? Paint::solid(Color::Black) : style.border,
                        style.borderWidth ? style.borderWidth : 1, style.radius, style.corners);
  if (props.checked) {
    const Paint ink = style.foreground.kind == PaintKind::None ? Paint::solid(Color::Black) : style.foreground;
    frame.target().line(Point{static_cast<int16_t>(boxRect.x + 3), static_cast<int16_t>(boxRect.y + box / 2)},
                        Point{static_cast<int16_t>(boxRect.x + box / 2 - 1),
                              static_cast<int16_t>(boxRect.bottom() - 4)},
                        2, ink);
    frame.target().line(Point{static_cast<int16_t>(boxRect.x + box / 2 - 1),
                              static_cast<int16_t>(boxRect.bottom() - 4)},
                        Point{static_cast<int16_t>(boxRect.right() - 3), static_cast<int16_t>(boxRect.y + 4)}, 2,
                        ink);
  }
  if (props.label) {
    Rect textRect{static_cast<int16_t>(boxRect.right() + props.gap), rect.y,
                  static_cast<int16_t>(rect.right() - boxRect.right() - props.gap), rect.height};
    Paint labelInk = hasState(state, StateDisabled) ? Paint::dither(Color::LightGray) : Paint::solid(Color::Black);
    frame.target().text(textRect, props.label, textStyleWithForeground(props.text, labelInk));
  }
}

template <size_t MaxInteractions>
void statusBar(Frame<MaxInteractions>& frame, Rect rect, const StatusBarProps& props) {
  if (props.fillBackground) frame.target().fill(rect, props.background);
  Rect textRect = rect.inset(Insets{0, props.horizontalPadding, 0, props.horizontalPadding});

  // Left cluster: icon, leading, leadingSecondary — measured and laid out in
  // sequence so the title never draws over them.
  const Paint ink = Paint::solid(props.text.color);  // icon matches text color in dark mode
  int16_t leftX = static_cast<int16_t>(textRect.x + props.leadingReserve);
  BitmapRef leadingIcon = props.leadingIcon ? props.leadingIcon : resolveBitmap(frame.assets(), props.leadingIconAsset);
  if (leadingIcon) {
    const int16_t iconSize = static_cast<int16_t>(leadingIcon.height > 0 ? leadingIcon.height : leadingIcon.width);
    Rect iconRect{leftX, static_cast<int16_t>(textRect.y + (textRect.height - iconSize) / 2), iconSize, iconSize};
    frame.target().bitmap(iconRect, leadingIcon, BitmapMode::Contain, ink);
    leftX = static_cast<int16_t>(leftX + iconSize + props.gap);
  }
  const char* leftItems[2] = {props.leading, props.leadingSecondary};
  for (const char* item : leftItems) {
    if (item == nullptr) continue;
    const int16_t itemW = frame.target().measureText(props.text.font, item, props.text).width;
    TextStyle left = props.text;
    left.align = TextAlign::Left;
    frame.target().text(Rect{leftX, textRect.y, itemW, textRect.height}, item, left);
    leftX = static_cast<int16_t>(leftX + itemW + props.gap);
  }

  int16_t trailingX = textRect.right();
  const char* rightItems[2] = {props.trailing, props.trailingSecondary};
  for (const char* item : rightItems) {
    if (item == nullptr) continue;
    const int16_t itemW = frame.target().measureText(props.text.font, item, props.text).width;
    TextStyle right = props.text;
    right.align = TextAlign::Right;
    trailingX = static_cast<int16_t>(trailingX - itemW);
    frame.target().text(Rect{trailingX, textRect.y, itemW, textRect.height}, item, right);
    trailingX = static_cast<int16_t>(trailingX - props.gap);
  }

  if (props.title) {
    // Prefer centering on the whole bar; fall back to centering between the
    // clusters when a wide cluster would collide with the title.
    TextStyle center = props.text;
    center.align = TextAlign::Center;
    const int16_t titleY = static_cast<int16_t>(textRect.y - props.titleOffsetY);
    const int16_t titleW = frame.target().measureText(props.text.font, props.title, props.text).width;
    int16_t titleX = static_cast<int16_t>(textRect.x + (textRect.width - titleW) / 2);
    if (titleX < leftX || static_cast<int16_t>(titleX + titleW) > trailingX) {
      Rect between{leftX, titleY, static_cast<int16_t>(trailingX - leftX), textRect.height};
      if (!between.empty()) frame.target().text(between, props.title, center);
    } else {
      frame.target().text(Rect{titleX, titleY, titleW, textRect.height}, props.title, center);
    }
  }
  if (props.showProgress) {
    Rect bar{rect.x, static_cast<int16_t>(rect.bottom() - props.progressHeight), rect.width, props.progressHeight};
    progressBar(frame, bar, props.progress);
  }
}

template <size_t MaxInteractions>
void popup(Frame<MaxInteractions>& frame, Rect rect, const PopupProps& props) {
  StyleSet styles = props.styles.unset() ? defaultPopupStyles() : props.styles;
  const BoxStyle& style = styles.resolve(StateNormal);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }
  Rect content = rect.inset(props.padding);
  if (props.showProgress) {
    const int16_t barH = props.progressHeight > 0 ? props.progressHeight : 4;
    Rect msg{content.x, content.y, content.width, static_cast<int16_t>(content.height - barH - 6)};
    frame.target().text(msg, props.message, props.text);
    progressBar(frame, Rect{content.x, static_cast<int16_t>(content.bottom() - barH), content.width, barH},
                props.progress);
  } else {
    frame.target().text(content, props.message, props.text);
  }
}

template <size_t MaxInteractions>
void textField(Frame<MaxInteractions>& frame, Rect rect, const TextFieldProps& props) {
  State state = props.disabled ? StateDisabled : (props.selected ? StateSelected : StateNormal);
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  const BoxStyle& style = styles.resolve(state);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }
  Rect content = rect.inset(Insets{0, props.horizontalPadding, 0, props.horizontalPadding});
  frame.target().text(content, props.text, props.textStyle);
  if (props.cursorVisible) {
    const int16_t lh = frame.target().lineHeight(props.textStyle.font);
    const int16_t cy = static_cast<int16_t>(content.y + (content.height - lh) / 2);
    int16_t cx = content.x;
    if (props.text && props.cursor > 0) {
      // Measure the prefix before the cursor in chunks so long inputs (URLs,
      // passphrases) place the cursor correctly without a large stack buffer.
      char buf[64];
      uint16_t consumed = 0;
      while (props.text[consumed] != '\0' && consumed < props.cursor) {
        uint16_t len = 0;
        while (props.text[consumed + len] != '\0' && consumed + len < props.cursor && len < sizeof(buf) - 1) {
          buf[len] = props.text[consumed + len];
          ++len;
        }
        // Never split a UTF-8 sequence across chunks: if the next byte is a
        // continuation byte, back off to the last sequence start.
        while (len > 1 && (props.text[consumed + len] & 0xC0) == 0x80 && (buf[len - 1] & 0x80) != 0) {
          --len;
        }
        if (len == 0) break;
        buf[len] = '\0';
        cx = static_cast<int16_t>(cx + frame.target().measureText(props.textStyle.font, buf, props.textStyle).width);
        consumed = static_cast<uint16_t>(consumed + len);
      }
    }
    if (props.cursorBlock) {
      frame.target().fill(Rect{cx, cy, 8, lh}, Paint::solid(Color::Black));
    } else {
      frame.target().fill(Rect{cx, cy, 2, lh}, Paint::solid(Color::Black));
    }
  }
}

// --- TextArea: multi-line scrollable writing canvas --------------------------
//
// A pure-render multi-line text surface (the editor body). The app owns the text
// buffer and caret offset and feeds it keystrokes; textArea word-wraps `text`
// into the rect, draws the window of visual lines starting at `topLine`, and (if
// showCaret) a thin caret at `cursor`. Unlike layoutText() it is top-aligned and
// has no line cap, so it scrolls arbitrarily long documents. Each source byte
// belongs to exactly one visual line (no spaces dropped) so a caret offset maps
// 1:1 to a (line, column). Keep the caret on screen with textAreaMeasure() +
// textAreaTopLineFor(), mirroring listTopIndexFor() for lists.
struct TextAreaProps {
  const char* text = nullptr;  // NUL-terminated, contiguous
  uint32_t cursor = 0;         // caret byte offset into text
  uint32_t topLine = 0;        // first visual line drawn at the top of the rect
  TextStyle style{};
  bool showCaret = true;
  // Selection highlight: byte range [selStart, selEnd). Empty (selStart>=selEnd)
  // means no selection. Drawn as a dithered band behind the text so it reads on
  // a 1-bit panel without inverting glyphs.
  uint32_t selStart = 0;
  uint32_t selEnd = 0;
};

// One wrapped visual line: byte range [start, start+len) of the source text plus
// whether it was terminated by an explicit '\n' (consumed, not part of len).
struct TextAreaLine {
  uint32_t start;
  uint16_t len;
  bool hardBreak;
};

// Walks `text` wrapped to `width`, calling emit(index, TextAreaLine) for each
// visual line in order; returns the total visual-line count. Empty text yields
// one empty line; a trailing '\n' yields a final empty line (so the caret has a
// line to sit on). Greedy word wrap, breaking after the last space that fits, or
// mid-word (codepoint-aligned) for words wider than the rect.
template <typename Emit>
inline uint32_t textAreaWalk(const DrawTarget& target, int16_t width, const char* text, const TextStyle& style,
                             Emit&& emit) {
  if (!text) text = "";
  if (width < 1) width = 1;
  char buf[224];
  const auto fits = [&](uint32_t start, uint32_t len) -> bool {
    uint32_t n = len < 220 ? len : 220;
    for (uint32_t i = 0; i < n; ++i) buf[i] = text[start + i];
    buf[n] = '\0';
    return target.measureText(style.font, buf, style).width <= width;
  };
  uint32_t total = 0;
  uint32_t lineStart = 0;
  for (;;) {
    uint32_t i = lineStart;
    uint32_t lastBreak = lineStart;  // byte after the last fitting space
    bool haveBreak = false;
    while (text[i] != '\0' && text[i] != '\n') {
      const uint32_t candidate = i + 1 - lineStart;
      if (candidate > 1 && !fits(lineStart, candidate)) break;  // would overflow
      if (text[i] == ' ') {
        lastBreak = i + 1;
        haveBreak = true;
      }
      ++i;
    }
    uint32_t lineEnd;
    uint32_t nextStart;
    bool hard = false;
    if (text[i] == '\n') {
      lineEnd = i;
      nextStart = i + 1;
      hard = true;
    } else if (text[i] == '\0') {
      lineEnd = i;
      nextStart = i;
    } else if (haveBreak && lastBreak > lineStart) {
      lineEnd = lastBreak;  // soft wrap after a space
      nextStart = lastBreak;
    } else {
      uint32_t brk = i > lineStart ? i : lineStart + 1;  // word wider than rect: char break
      while (brk > lineStart + 1 && (text[brk] & 0xC0) == 0x80) --brk;  // keep codepoints whole
      lineEnd = brk;
      nextStart = brk;
    }
    emit(total, TextAreaLine{lineStart, static_cast<uint16_t>(lineEnd - lineStart), hard});
    ++total;
    if (!hard && text[lineEnd] == '\0') break;  // end of buffer, no trailing newline
    if (hard && text[nextStart] == '\0') {
      emit(total, TextAreaLine{nextStart, 0, false});  // trailing newline -> empty final line
      ++total;
      break;
    }
    lineStart = nextStart;
  }
  return total;
}

struct TextAreaMetrics {
  uint32_t lineCount = 0;  // total visual lines
  uint32_t caretLine = 0;  // visual line containing `cursor`
};

// Total visual lines and the line the caret sits on, for scroll math.
inline TextAreaMetrics textAreaMeasure(const DrawTarget& target, int16_t width, const char* text,
                                       const TextStyle& style, uint32_t cursor) {
  TextAreaMetrics m;
  bool found = false;
  m.lineCount = textAreaWalk(target, width, text, style, [&](uint32_t idx, const TextAreaLine& ln) {
    if (!found && cursor >= ln.start && cursor <= static_cast<uint32_t>(ln.start + ln.len)) {
      m.caretLine = idx;
      found = true;
    }
  });
  if (!found) m.caretLine = m.lineCount > 0 ? m.lineCount - 1 : 0;
  return m;
}

// Visual lines that fully fit in the rect at the given line height.
inline uint16_t textAreaVisibleLines(Rect rect, int16_t lineHeight) {
  if (rect.height <= 0 || lineHeight <= 0) return 0;
  return static_cast<uint16_t>(rect.height / lineHeight);
}

// Adjusts topLine the minimal amount so caretLine is visible, then clamps to the
// valid scroll range — the textArea analogue of listTopIndexFor().
inline uint32_t textAreaTopLineFor(uint32_t caretLine, uint32_t topLine, uint16_t visibleLines, uint32_t lineCount) {
  if (visibleLines == 0) return 0;
  const uint32_t maxTop = lineCount > visibleLines ? lineCount - visibleLines : 0;
  uint32_t top = topLine > maxTop ? maxTop : topLine;
  if (caretLine < top) {
    top = caretLine;
  } else if (caretLine >= top + visibleLines) {
    top = caretLine - visibleLines + 1;
  }
  return top > maxTop ? maxTop : top;
}

template <size_t MaxInteractions>
void textArea(Frame<MaxInteractions>& frame, Rect rect, const TextAreaProps& props) {
  DrawTarget& t = frame.target();
  if (rect.empty()) return;
  const int16_t lh = t.lineHeight(props.style.font);
  if (lh <= 0) return;
  const uint16_t visible = static_cast<uint16_t>(rect.height / lh);
  if (visible == 0) return;
  const char* text = props.text ? props.text : "";
  const uint32_t top = props.topLine;
  TextStyle lineStyle = props.style;
  lineStyle.maxLines = 1;
  lineStyle.align = TextAlign::Left;
  char buf[224];
  bool caretDrawn = false;
  const bool hasSel = props.selEnd > props.selStart;
  textAreaWalk(t, rect.width, text, props.style, [&](uint32_t idx, const TextAreaLine& ln) {
    if (idx < top || idx >= top + visible) return;
    const int16_t y = static_cast<int16_t>(rect.y + (idx - top) * lh);
    uint16_t n = ln.len < 220 ? ln.len : 220;
    const uint32_t lineEnd = ln.start + ln.len;

    // Selection band behind the text. Dithered so 1-bit glyphs stay readable.
    if (hasSel && props.selStart <= lineEnd && props.selEnd > ln.start) {
      uint32_t ovS = props.selStart > ln.start ? props.selStart : ln.start;
      uint16_t ps = static_cast<uint16_t>(ovS - ln.start);
      if (ps > n) ps = n;
      for (uint16_t i = 0; i < ps; ++i) buf[i] = text[ln.start + i];
      buf[ps] = '\0';
      const int16_t xs = static_cast<int16_t>(rect.x + t.measureText(props.style.font, buf, props.style).width);
      int16_t xe;
      if (props.selEnd > lineEnd) {
        xe = rect.right();  // selection continues onto the next line: show the line break selected
      } else {
        uint16_t pe = static_cast<uint16_t>(props.selEnd - ln.start);
        if (pe > n) pe = n;
        for (uint16_t i = 0; i < pe; ++i) buf[i] = text[ln.start + i];
        buf[pe] = '\0';
        xe = static_cast<int16_t>(rect.x + t.measureText(props.style.font, buf, props.style).width);
      }
      if (xe > xs) t.fill(Rect{xs, y, static_cast<int16_t>(xe - xs), lh}, Paint::dither(Color::LightGray));
    }

    for (uint16_t i = 0; i < n; ++i) buf[i] = text[ln.start + i];
    buf[n] = '\0';
    if (n > 0) t.text(Rect{rect.x, y, rect.width, lh}, buf, lineStyle);
    if (props.showCaret && !caretDrawn && props.cursor >= ln.start &&
        props.cursor <= static_cast<uint32_t>(ln.start + ln.len)) {
      caretDrawn = true;
      uint16_t pre = static_cast<uint16_t>(props.cursor - ln.start);
      if (pre > n) pre = n;
      for (uint16_t i = 0; i < pre; ++i) buf[i] = text[ln.start + i];
      buf[pre] = '\0';
      const int16_t cx = static_cast<int16_t>(rect.x + t.measureText(props.style.font, buf, props.style).width);
      t.fill(Rect{cx, y, 2, lh}, Paint::solid(Color::Black));
    }
  });
}

template <size_t MaxInteractions>
void dropdown(Frame<MaxInteractions>& frame, Rect rect, const DropdownProps& props) {
  State state = props.enabled ? props.state : static_cast<State>(props.state | StateDisabled);
  if (props.enabled && props.action != NO_ACTION) {
    frame.hit(ensureMinTouchRect(rect, props.minTouchSize, frame.screen()), props.action, props.actionValue,
              props.inputMask, state);
  }
  state = frame.stateFor(props.action, props.actionValue, state);
  StyleSet styles = props.styles.unset() ? defaultButtonStyles() : props.styles;
  if (props.radius > 0) setStyleRadius(styles, props.radius);
  const BoxStyle& style = styles.resolve(state);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  const int16_t indicatorW = props.indicatorWidth < 8 ? 8 : props.indicatorWidth;
  Rect indicator{static_cast<int16_t>(content.right() - indicatorW), content.y, indicatorW, content.height};
  const int16_t cx = static_cast<int16_t>(indicator.x + indicator.width / 2);
  const int16_t cy = static_cast<int16_t>(indicator.y + indicator.height / 2);
  const Paint ink = style.foreground.kind == PaintKind::None ? Paint::solid(Color::Black) : style.foreground;
  const int16_t chevron = props.indicatorSize < 4 ? 4 : props.indicatorSize;
  const int16_t half = static_cast<int16_t>(chevron / 2);
  const int16_t rise = half;
  const uint8_t stroke = props.indicatorStroke == 0 ? 1 : props.indicatorStroke;
  frame.target().line(Point{static_cast<int16_t>(cx - half), static_cast<int16_t>(cy - rise)},
                      Point{cx, cy}, stroke, ink);
  frame.target().line(Point{cx, cy}, Point{static_cast<int16_t>(cx + half), static_cast<int16_t>(cy - rise)},
                      stroke, ink);

  Rect textRect{content.x, content.y, static_cast<int16_t>(indicator.x - content.x - props.gap), content.height};
  if (props.label && props.value) {
    const Size labelSize = frame.target().measureText(props.labelText.font, props.label, props.labelText);
    Rect labelRect{textRect.x, textRect.y, labelSize.width, textRect.height};
    frame.target().text(labelRect, props.label, textStyleWithForeground(props.labelText, ink));
    Rect valueRect{static_cast<int16_t>(labelRect.right() + props.gap), textRect.y,
                   static_cast<int16_t>(textRect.right() - labelRect.right() - props.gap), textRect.height};
    frame.target().text(valueRect, props.value, textStyleWithForeground(props.valueText, ink));
  } else if (props.value || props.label) {
    frame.target().text(textRect, props.value ? props.value : props.label,
                        textStyleWithForeground(props.valueText, ink));
  }
}

template <size_t MaxInteractions>
void table(Frame<MaxInteractions>& frame, Rect rect, const TableProps& props) {
  if (!props.cells || props.rows == 0 || props.cols == 0) return;
  StyleSet styles = props.cellStyles.unset() ? defaultListRowStyles() : props.cellStyles;
  if (props.cellRadius > 0) setStyleRadius(styles, props.cellRadius);
  const int16_t cellW = static_cast<int16_t>(rect.width / props.cols);
  const int16_t rowH = props.rowHeight < 1 ? static_cast<int16_t>(rect.height / props.rows) : props.rowHeight;
  for (uint8_t row = 0; row < props.rows; ++row) {
    int16_t x = rect.x;
    for (uint8_t col = 0; col < props.cols; ++col) {
      const bool lastCol = col == props.cols - 1;
      Rect cell{x, static_cast<int16_t>(rect.y + row * rowH),
                lastCol ? static_cast<int16_t>(rect.right() - x) : cellW, rowH};
      State state = props.headerRow && row == 0 ? StateSelected : StateNormal;
      const BoxStyle& style = styles.resolve(state);
      frame.target().fill(cell, style.background, style.radius, style.corners);
      if (props.grid.kind != PaintKind::None && props.gridWidth > 0) {
        frame.target().stroke(cell, props.grid, props.gridWidth, style.radius, style.corners);
      }
      const char* value = props.cells[static_cast<uint16_t>(row) * props.cols + col];
      if (value) {
        Rect textRect = cell.inset(Insets{0, props.padding, 0, props.padding});
        frame.target().text(textRect, value, textStyleWithForeground(props.text, style.foreground));
      }
      x = static_cast<int16_t>(x + cell.width);
    }
  }
}

template <size_t MaxInteractions>
void list(Frame<MaxInteractions>& frame, Rect rect, const ListProps& props) {
  if (!props.items || props.count == 0) return;
  const int16_t rowH = props.rowHeight > 0 ? props.rowHeight : 36;
  const uint16_t visible = listVisibleRows(rect, rowH, props.rowGap);
  const bool overflows = props.count > visible;
  uint16_t top = props.topIndex;
  if (top > props.count - 1) top = props.count - 1;
  if (overflows && top > props.count - visible) top = static_cast<uint16_t>(props.count - visible);
  if (!overflows) top = 0;
  const uint16_t end = overflows ? static_cast<uint16_t>(top + visible) : props.count;

  Rect rowArea = rect;
  if (props.scrollIndicator && overflows && props.scrollIndicatorWidth > 0) {
    rowArea.width = static_cast<int16_t>(rowArea.width - props.scrollIndicatorWidth - 2);
    Rect track{static_cast<int16_t>(rect.right() - props.scrollIndicatorWidth), rect.y, props.scrollIndicatorWidth,
               rect.height};
    frame.target().fill(track, Paint::dither(Color::LightGray));
    int16_t thumbH = static_cast<int16_t>((static_cast<int32_t>(rect.height) * visible) / props.count);
    if (thumbH < 12) thumbH = 12;
    const int32_t scrollRange = props.count - visible;
    const int16_t thumbY = static_cast<int16_t>(
        track.y + (scrollRange > 0 ? (static_cast<int32_t>(track.height - thumbH) * top) / scrollRange : 0));
    frame.target().fill(Rect{track.x, thumbY, track.width, thumbH}, Paint::solid(Color::Black));
  }

  // Cursor-based layout: section header rows are shorter than item rows, so
  // positions accumulate instead of multiplying a fixed stride.
  const int16_t headerLh = frame.target().lineHeight(props.headerText.font);
  const int16_t headerH = props.headerRowHeight > 0 ? props.headerRowHeight : static_cast<int16_t>(headerLh + 4);
  int16_t cursorY = rowArea.y;
  uint16_t drawnRows = 0;
  for (uint16_t i = top; i < props.count; ++i) {
    const ListItem& item = props.items[i];
    if (item.isHeader) {
      const int16_t pad = i != top ? props.sectionGap : 0;
      if (static_cast<int16_t>(cursorY + pad + headerH) > rowArea.bottom()) break;
      cursorY = static_cast<int16_t>(cursorY + pad);
      Rect headerRow{static_cast<int16_t>(rowArea.x + props.sidePadding), cursorY,
                     static_cast<int16_t>(rowArea.width - props.sidePadding * 2), headerLh};
      frame.target().text(headerRow, item.label, props.headerText);
      if (props.headerUnderline) {
        frame.target().fill(Rect{headerRow.x, static_cast<int16_t>(cursorY + headerLh + 2), headerRow.width, 1},
                            Paint::solid(props.headerText.color));
      }
      cursorY = static_cast<int16_t>(cursorY + headerH + props.rowGap);
      continue;
    }
    if (static_cast<int16_t>(cursorY + rowH) > rowArea.bottom() || drawnRows >= visible || i >= end) break;
    ++drawnRows;
    Rect row{rowArea.x, cursorY, rowArea.width, rowH};
    cursorY = static_cast<int16_t>(cursorY + rowH + props.rowGap);
    if (props.hugContents && item.label) {
      // Hug-content rows shrink to the label width plus padding so the
      // selection pill wraps the text instead of spanning the rect.
      const int16_t labelW = frame.target().measureText(props.labelText.font, item.label, props.labelText).width;
      const int16_t hugW = static_cast<int16_t>(labelW + props.sidePadding * 2);
      if (hugW < row.width) row.width = hugW;
    }
    State state = item.state;
    if (props.selectedIndex == static_cast<int16_t>(i)) state |= StateSelected;
    if (!item.enabled) state |= StateDisabled;
    if (props.action != NO_ACTION && item.enabled) {
      frame.hit(ensureMinTouchRect(row, frame.device().minTouchSize, frame.screen()), props.action, item.actionValue,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, item.actionValue, state);
    StyleSet styles = props.rowStyles.unset() ? defaultListRowStyles() : props.rowStyles;
    if (props.rowRadius > 0) setStyleRadius(styles, props.rowRadius);
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(row, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(row, style.border, style.borderWidth, style.radius, style.corners);
    }

    Rect content = row.inset(Insets{0, props.sidePadding, 0, props.sidePadding});
    const BitmapRef icon = item.icon ? item.icon : resolveBitmap(frame.assets(), item.iconAsset);
    if (icon) {
      const int16_t iconSize = props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon.width);
      Rect iconRect{content.x, static_cast<int16_t>(content.y + (content.height - iconSize) / 2), iconSize, iconSize};
      frame.target().bitmap(iconRect, icon, BitmapMode::Contain, style.foreground);
      content.x = static_cast<int16_t>(content.x + iconSize + props.textGap);
      content.width = static_cast<int16_t>(content.width - iconSize - props.textGap);
    }
    if (item.value) {
      TextStyle valueStyle = textStyleWithForeground(props.valueText, style.foreground);
      valueStyle.align = TextAlign::Right;
      const int16_t valueW = frame.target().measureText(valueStyle.font, item.value, valueStyle).width;
      Rect valueRect{static_cast<int16_t>(content.right() - valueW), content.y, valueW, content.height};
      frame.target().text(valueRect, item.value, valueStyle);
      content.width = static_cast<int16_t>(content.width - valueW - props.textGap);
    }
    if (item.subtitle) {
      const int16_t lh = frame.target().lineHeight(props.labelText.font);
      Rect labelRect{content.x, content.y, content.width, lh};
      Rect subRect{content.x, static_cast<int16_t>(content.y + lh), content.width,
                   static_cast<int16_t>(content.height - lh)};
      frame.target().text(labelRect, item.label, textStyleWithForeground(props.labelText, style.foreground));
      frame.target().text(subRect, item.subtitle, textStyleWithForeground(props.subtitleText, style.foreground));
    } else {
      TextStyle labelStyle = textStyleWithForeground(props.labelText, style.foreground);
      if (props.centerSingleLine) labelStyle.align = TextAlign::Center;
      frame.target().text(content, item.label, labelStyle);
    }

    if (props.selectedIndex == static_cast<int16_t>(i) && props.selectionMarker != SelectionMarker::None) {
      if (props.selectionMarker == SelectionMarker::Underline) {
        frame.target().fill(Rect{static_cast<int16_t>(row.x + props.sidePadding + props.markerInset),
                                 static_cast<int16_t>(row.bottom() - props.markerThickness),
                                 static_cast<int16_t>(row.width - props.sidePadding * 2 - props.markerInset),
                                 props.markerThickness},
                            props.markerPaint);
      } else {
        // 12x18 right-pointing triangle, vertically centered — the v1 theme
        // Triangle selection marker geometry.
        const int16_t tx = static_cast<int16_t>(row.x + props.markerInset);
        const int16_t cy = static_cast<int16_t>(row.y + row.height / 2);
        frame.target().triangle(Point{tx, static_cast<int16_t>(cy - 9)}, Point{tx, static_cast<int16_t>(cy + 9)},
                                Point{static_cast<int16_t>(tx + 12), cy}, props.markerPaint);
      }
    }
  }
}

template <size_t MaxInteractions>
void keyGrid(Frame<MaxInteractions>& frame, Rect rect, const KeyGridProps& props) {
  if (!props.keys || props.rows == 0 || props.cols == 0) return;
  const int16_t cellW = static_cast<int16_t>((rect.width - (props.cols - 1) * props.gap) / props.cols);
  const int16_t cellH = static_cast<int16_t>((rect.height - (props.rows - 1) * props.gap) / props.rows);
  for (uint8_t row = 0; row < props.rows; ++row) {
    for (uint8_t col = 0; col < props.cols; ++col) {
      const uint8_t idx = static_cast<uint8_t>(row * props.cols + col);
      const KeyGridKey& key = props.keys[idx];
      Rect keyRect{static_cast<int16_t>(rect.x + col * (cellW + props.gap)),
                   static_cast<int16_t>(rect.y + row * (cellH + props.gap)), cellW, cellH};
      State state = key.state;
      if (props.selectedIndex == idx) state |= props.inactiveSelection ? StateFocused : StateSelected;
      if (!key.enabled || key.kind == KeyKind::Disabled) state |= StateDisabled;
      ButtonProps bp;
      bp.label = key.label;
      bp.icon = key.icon ? key.icon : resolveBitmap(frame.assets(), key.iconAsset);
      bp.action = props.action;
      bp.value = key.value;
      bp.inputMask = props.inputMask;
      bp.state = state;
      bp.text = props.labelText;
      bp.styles = props.keyStyles.unset() ? defaultKeyStyles() : props.keyStyles;
      bp.minTouchSize = props.minTouchSize;
      bp.radius = props.radius;
      bp.enabled = key.enabled && key.kind != KeyKind::Disabled;
      if (key.kind == KeyKind::Delete && !bp.icon) {
        bp.icon = lucideDeleteIcon16();
        bp.label = nullptr;
      }
      // Space renders glyph art instead of a text label.
      const bool glyphKey = key.kind == KeyKind::Space && !bp.icon;
      if (glyphKey) bp.label = nullptr;
      button(frame, keyRect, bp);
      if (glyphKey) {
        const Paint ink = bp.styles.resolve(frame.stateFor(props.action, key.value, state)).foreground;
        const int16_t cx = static_cast<int16_t>(keyRect.x + keyRect.width / 2);
        const int16_t cy = static_cast<int16_t>(keyRect.y + keyRect.height / 2);
        const int16_t half = static_cast<int16_t>(keyRect.width * 3 / 10);
        frame.target().line(Point{static_cast<int16_t>(cx - half), static_cast<int16_t>(cy + 3)},
                            Point{static_cast<int16_t>(cx + half), static_cast<int16_t>(cy + 3)}, 3, ink);
      }
      if (key.secondaryLabel && key.enabled) {
        Rect sec{static_cast<int16_t>(keyRect.right() - cellW / 3), keyRect.y, static_cast<int16_t>(cellW / 3),
                 static_cast<int16_t>(cellH / 2)};
        frame.target().text(sec, key.secondaryLabel, props.secondaryText);
      }
    }
  }
}

template <size_t MaxInteractions>
void keyboard(Frame<MaxInteractions>& frame, Rect rect, const KeyboardProps& props) {
  if (!props.layout || !props.layout->rows || props.layout->rowCount == 0) return;
  StyleSet styles = props.keyStyles.unset() ? defaultButtonStyles() : props.keyStyles;
  if (props.keyRadius > 0) setStyleRadius(styles, props.keyRadius);
  TextStyle keyText = props.labelText;
  keyText.align = TextAlign::Center;
  keyText.maxLines = 1;
  rect = rect.inset(props.padding);
  if (rect.empty() || rect.width < 10 || rect.height < 10) return;
  const int16_t gap = props.gap < 0 ? 0 : props.gap;
  const int16_t rowH = static_cast<int16_t>((rect.height - gap * (props.layout->rowCount - 1)) / props.layout->rowCount);
  int16_t logicalIndex = 0;

  auto actionFor = [&](KeyKind kind) {
    if (kind == KeyKind::Shift && props.shiftAction != NO_ACTION) return props.shiftAction;
    if (kind == KeyKind::Mode && props.modeAction != NO_ACTION) return props.modeAction;
    if (kind == KeyKind::Delete && props.deleteAction != NO_ACTION) return props.deleteAction;
    if (kind == KeyKind::Ok && props.okAction != NO_ACTION) return props.okAction;
    return props.keyAction;
  };

  auto drawKey = [&](Rect keyRect, const KeyboardKey& key, int16_t selectedIndex) {
    State state = StateNormal;
    if (props.selectedIndex == selectedIndex) state |= props.inactiveSelection ? StateFocused : StateSelected;
    if (!key.enabled || key.kind == KeyKind::Disabled) state |= StateDisabled;
    const ActionId action = actionFor(key.kind);
    ButtonProps bp;
    bp.label = (key.kind == KeyKind::Space || key.kind == KeyKind::Delete) ? nullptr : key.label;
    bp.action = action;
    bp.value = key.value;
    bp.inputMask = props.inputMask;
    bp.state = state;
    bp.text = keyText;
    bp.styles = styles;
    bp.minTouchSize = props.minTouchSize;
    bp.radius = props.keyRadius;
    bp.enabled = key.enabled && key.kind != KeyKind::Disabled;
    if (key.kind == KeyKind::Delete) bp.icon = lucideDeleteIcon16();
    button(frame, keyRect, bp);

    if (key.kind != KeyKind::Space) return;
    const Paint ink = styles.resolve(frame.stateFor(action, key.value, state)).foreground;
    const int16_t cx = static_cast<int16_t>(keyRect.x + keyRect.width / 2);
    const int16_t cy = static_cast<int16_t>(keyRect.y + keyRect.height / 2);
    const int16_t half = static_cast<int16_t>(keyRect.width * 3 / 10);
    frame.target().line(Point{static_cast<int16_t>(cx - half), static_cast<int16_t>(cy + 3)},
                        Point{static_cast<int16_t>(cx + half), static_cast<int16_t>(cy + 3)}, 3, ink);
  };

  for (uint8_t row = 0; row < props.layout->rowCount; ++row) {
    const KeyboardRow& layoutRow = props.layout->rows[row];
    if (!layoutRow.keys || layoutRow.count == 0) continue;
    uint16_t units = static_cast<uint16_t>(layoutRow.insetUnits * 2);
    for (uint8_t col = 0; col < layoutRow.count; ++col) {
      units = static_cast<uint16_t>(units + (layoutRow.keys[col].widthUnits ? layoutRow.keys[col].widthUnits : 1));
    }
    const int16_t unitW = static_cast<int16_t>((rect.width - gap * (layoutRow.count - 1)) / units);
    const int16_t y = static_cast<int16_t>(rect.y + row * (rowH + gap));
    int16_t x = static_cast<int16_t>(rect.x + layoutRow.insetUnits * unitW);
    const int16_t rowRight = static_cast<int16_t>(rect.right() - layoutRow.insetUnits * unitW);
    for (uint8_t col = 0; col < layoutRow.count; ++col) {
      const KeyboardKey& key = layoutRow.keys[col];
      const uint8_t keyUnits = key.widthUnits ? key.widthUnits : 1;
      const int16_t w = col == layoutRow.count - 1 ? static_cast<int16_t>(rowRight - x)
                                                   : static_cast<int16_t>(unitW * keyUnits);
      drawKey(Rect{x, y, w, rowH}, key, logicalIndex++);
      x = static_cast<int16_t>(x + w + gap);
    }
  }
}

template <size_t MaxInteractions>
void qwertyKeyboard(Frame<MaxInteractions>& frame, Rect rect, const QwertyKeyboardProps& props) {
  KeyboardProps keyboardProps;
  keyboardProps.layout = &builtinKeyboardLayout(props.layout, props.shifted, props.symbols);
  keyboardProps.keyAction = props.keyAction;
  keyboardProps.shiftAction = props.shiftAction;
  keyboardProps.modeAction = props.modeAction;
  keyboardProps.deleteAction = props.deleteAction;
  keyboardProps.okAction = props.okAction;
  keyboardProps.inputMask = props.inputMask;
  keyboardProps.selectedIndex = props.selectedIndex;
  keyboardProps.labelText = props.labelText;
  keyboardProps.keyStyles = props.keyStyles;
  keyboardProps.padding = props.padding;
  keyboardProps.gap = props.gap;
  keyboardProps.minTouchSize = props.minTouchSize;
  keyboardProps.keyRadius = props.keyRadius;
  keyboardProps.inactiveSelection = props.inactiveSelection;
  keyboard(frame, rect, keyboardProps);
}

template <size_t MaxInteractions, size_t MaxSlots = 3>
void gestureBar(Frame<MaxInteractions>& frame, Rect rect, const GestureBarProps& props) {
  Stack<MaxSlots> row(rect, Axis::Row, props.gap);
  row.flex(1);
  row.flex(1);
  row.flex(1);
  row.layout();
  const GestureBarButton buttons[3] = {props.left, props.center, props.right};
  for (uint8_t i = 0; i < 3; ++i) {
    const GestureBarButton& item = buttons[i];
    if (item.action == NO_ACTION && !item.label && !item.icon) continue;
    ButtonProps bp;
    bp.label = item.label;
    bp.icon = item.icon;
    bp.iconAsset = item.iconAsset;
    bp.action = item.action;
    bp.value = item.value;
    bp.inputMask = props.inputMask;
    bp.state = item.state;
    bp.text = props.text;
    bp.styles = props.styles;
    bp.minTouchSize = props.height;
    button(frame, row.rect(i), bp);
  }
  if (props.swipeLeft != NO_ACTION) {
    frame.hit(rect, props.swipeLeft, 0, InputSwipeLeft, StateNormal);
  }
  if (props.swipeRight != NO_ACTION) {
    frame.hit(rect, props.swipeRight, 0, InputSwipeRight, StateNormal);
  }
}

// Lays out a prev/center/next cover carousel, draws the frames and selection
// chrome, and routes taps (cover → its index), prev/next buttons, and swipes
// (→ neighbor indexes). Returns the slots in `slots[3]` (0 = previous,
// 1 = center, 2 = next) so the app renders cover art into each
// `slots[i].content`.
template <size_t MaxInteractions>
void coverCarousel(Frame<MaxInteractions>& frame, Rect rect, const CarouselProps& props, CarouselSlot slots[3]) {
  for (int i = 0; i < 3; ++i) slots[i] = CarouselSlot{};
  if (props.count == 0) return;

  const auto neighbor = [&](const int16_t delta) -> int16_t {
    const int32_t raw = props.selectedIndex + delta;
    if (raw >= 0 && raw < props.count) return static_cast<int16_t>(raw);
    if (!props.wrap || props.count < 2) return -1;
    return static_cast<int16_t>((raw + props.count) % props.count);
  };
  slots[0].itemIndex = neighbor(-1);
  slots[1].itemIndex = props.selectedIndex;
  slots[1].isCenter = true;
  slots[2].itemIndex = neighbor(1);

  const int16_t centerX = static_cast<int16_t>(rect.x + (rect.width - props.centerSize.width) / 2);
  const int16_t midY = static_cast<int16_t>(rect.y + rect.height / 2);
  slots[1].frame = Rect{centerX, static_cast<int16_t>(midY - props.centerSize.height / 2), props.centerSize.width,
                        props.centerSize.height};
  slots[0].frame = Rect{static_cast<int16_t>(centerX - props.gap - props.sideSize.width),
                        static_cast<int16_t>(midY - props.sideSize.height / 2), props.sideSize.width,
                        props.sideSize.height};
  slots[2].frame = Rect{static_cast<int16_t>(slots[1].frame.right() + props.gap),
                        static_cast<int16_t>(midY - props.sideSize.height / 2), props.sideSize.width,
                        props.sideSize.height};

  StyleSet styles = props.frameStyles;
  if (styles.unset()) {
    styles.normal.background = Paint::solid(Color::White);
    styles.normal.border = Paint::solid(Color::Black);
    styles.normal.borderWidth = 1;
    styles.selected = styles.normal;
    styles.selected.borderWidth = 3;
  }

  for (int i = 0; i < 3; ++i) {
    CarouselSlot& slot = slots[i];
    if (slot.itemIndex < 0) continue;
    slot.valid = true;
    slot.content = slot.frame.inset(
        Insets{props.contentInset, props.contentInset, props.contentInset, props.contentInset});
    if (props.action != NO_ACTION) {
      frame.hit(ensureMinTouchRect(slot.frame, frame.device().minTouchSize, frame.screen()), props.action,
                slot.itemIndex, props.inputMask, slot.isCenter ? StateSelected : StateNormal);
    }
    const BoxStyle& style = styles.resolve(slot.isCenter ? StateSelected : StateNormal);
    frame.target().fill(slot.frame, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(slot.frame, style.border, style.borderWidth, style.radius, style.corners);
    }
  }

  // Swipes and prev/next buttons step to the neighbors.
  if (props.action != NO_ACTION) {
    if (slots[0].itemIndex >= 0) {
      frame.hit(rect, props.action, slots[0].itemIndex, InputSwipeRight | InputPrev, StateNormal);
    }
    if (slots[2].itemIndex >= 0) {
      frame.hit(rect, props.action, slots[2].itemIndex, InputSwipeLeft | InputNext, StateNormal);
    }
  }
}

template <size_t MaxInteractions>
void tabBar(Frame<MaxInteractions>& frame, Rect rect, const TabBarProps& props) {
  if (!props.tabs || props.count == 0) return;
  StyleSet styles = props.tabStyles;
  if (styles.unset()) {
    styles.selected.background = Paint::solid(Color::Black);
    styles.selected.foreground = Paint::solid(Color::White);
  }

  const int16_t dividerH = props.divider ? 1 : 0;
  const int16_t slotW = static_cast<int16_t>(rect.width / props.count);
  for (uint8_t i = 0; i < props.count; ++i) {
    const TabItem& tab = props.tabs[i];
    Rect slot{static_cast<int16_t>(rect.x + i * slotW), rect.y, slotW,
              static_cast<int16_t>(rect.height - dividerH)};
    Rect pill = slot.inset(props.tabInset);
    State state = tab.selected ? StateSelected : StateNormal;
    if (props.action != NO_ACTION) {
      frame.hit(ensureMinTouchRect(slot, frame.device().minTouchSize, frame.screen()), props.action, tab.value,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, tab.value, state);
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(pill, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(pill, style.border, style.borderWidth, style.radius, style.corners);
    }
    TextStyle label = textStyleWithForeground(props.text, style.foreground);
    label.align = TextAlign::Center;
    frame.target().text(pill, tab.label, label);
  }
  if (props.divider) {
    frame.target().fill(Rect{rect.x, static_cast<int16_t>(rect.bottom() - 1), rect.width, 1}, props.dividerPaint);
  }
}

template <size_t MaxInteractions>
void batteryIndicator(Frame<MaxInteractions>& frame, Rect rect, const BatteryIndicatorProps& props) {
  const int16_t glyphH = props.glyphHeight < 6 ? 6 : props.glyphHeight;
  const int16_t glyphW = props.glyphWidth < 8 ? 8 : props.glyphWidth;
  const int16_t nubW = 2;
  const int16_t nubH = static_cast<int16_t>(glyphH / 2);
  Rect body{static_cast<int16_t>(rect.right() - glyphW - nubW), static_cast<int16_t>(rect.y + (rect.height - glyphH) / 2),
            glyphW, glyphH};
  const Paint ink = Paint::solid(props.color);

  frame.target().stroke(body, ink, 1);
  frame.target().fill(Rect{body.right(), static_cast<int16_t>(body.y + (glyphH - nubH) / 2), nubW, nubH}, ink);

  const uint8_t percent = props.percent > 100 ? 100 : props.percent;
  Rect cavity = body.inset(Insets{2, 2, 2, 2});
  const int16_t fillW = static_cast<int16_t>((static_cast<int32_t>(cavity.width) * percent) / 100);
  if (fillW > 0) {
    frame.target().fill(Rect{cavity.x, cavity.y, fillW, cavity.height}, ink);
  }
  if (props.charging) {
    if (props.chargingIcon) {
      frame.target().bitmap(centeredRect(body, Size{static_cast<int16_t>(props.chargingIcon.width),
                                                    static_cast<int16_t>(props.chargingIcon.height)}),
                            props.chargingIcon, BitmapMode::Center, ink);
    } else {
      // Lightning bolt from two triangles, drawn in the inverse color so it
      // reads over both the filled and empty parts of the cavity.
      const Paint bolt = Paint::solid(invertedColor(props.color));
      const int16_t cx = static_cast<int16_t>(cavity.x + cavity.width / 2);
      const int16_t cy = static_cast<int16_t>(cavity.y + cavity.height / 2);
      const int16_t h = static_cast<int16_t>(cavity.height - 2);
      const int16_t w = static_cast<int16_t>(h / 2 > 2 ? h / 2 : 2);
      frame.target().triangle(Point{static_cast<int16_t>(cx + w / 2), static_cast<int16_t>(cy - h / 2)},
                              Point{static_cast<int16_t>(cx - w), static_cast<int16_t>(cy + 1)},
                              Point{static_cast<int16_t>(cx + 1), static_cast<int16_t>(cy + 1)}, bolt);
      frame.target().triangle(Point{static_cast<int16_t>(cx - w / 2), static_cast<int16_t>(cy + h / 2)},
                              Point{static_cast<int16_t>(cx + w), static_cast<int16_t>(cy - 1)},
                              Point{static_cast<int16_t>(cx - 1), static_cast<int16_t>(cy - 1)}, bolt);
    }
  }
  if (props.label) {
    TextStyle style = props.text;
    style.align = TextAlign::Right;
    Rect labelRect{rect.x, rect.y, static_cast<int16_t>(body.x - props.gap - rect.x), rect.height};
    drawText(frame.target(), labelRect, props.label, style);
  }
}

template <size_t MaxInteractions>
void metricCard(Frame<MaxInteractions>& frame, Rect rect, const MetricCardProps& props) {
  if (props.action != NO_ACTION) {
    frame.hit(ensureMinTouchRect(rect, frame.device().minTouchSize, frame.screen()), props.action, props.actionValue,
              props.inputMask, props.state);
  }
  StyleSet styles = props.styles.unset() ? defaultPopupStyles() : props.styles;
  const BoxStyle& style = styles.resolve(frame.stateFor(props.action, props.actionValue, props.state));
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  const TextAlign align = props.centered ? TextAlign::Center : TextAlign::Left;
  int16_t cursorY = content.y;
  if (props.label) {
    TextStyle labelStyle = textStyleWithForeground(props.labelText, style.foreground);
    labelStyle.align = align;
    const int16_t lh = frame.target().lineHeight(labelStyle.font);
    drawText(frame.target(), Rect{content.x, cursorY, content.width, lh}, props.label, labelStyle);
    cursorY = static_cast<int16_t>(cursorY + lh + props.gap);
  }
  int16_t captionH = 0;
  if (props.caption) {
    TextStyle captionStyle = textStyleWithForeground(props.captionText, style.foreground);
    captionStyle.align = align;
    captionH = frame.target().lineHeight(captionStyle.font);
    drawText(frame.target(),
             Rect{content.x, static_cast<int16_t>(content.bottom() - captionH), content.width, captionH},
             props.caption, captionStyle);
  }
  if (props.value) {
    TextStyle valueStyle = textStyleWithForeground(props.valueText, style.foreground);
    valueStyle.align = align;
    Rect valueRect{content.x, cursorY, content.width,
                   static_cast<int16_t>(content.bottom() - captionH - (captionH ? props.gap : 0) - cursorY)};
    if (props.unit) {
      // Draw value+unit as one centered group: value in its font, unit small
      // and right after it.
      TextStyle unitStyle = textStyleWithForeground(props.labelText, style.foreground);
      unitStyle.align = TextAlign::Left;
      const Size valueSize = frame.target().measureText(valueStyle.font, props.value, valueStyle);
      const Size unitSize = frame.target().measureText(unitStyle.font, props.unit, unitStyle);
      const int16_t groupW = static_cast<int16_t>(valueSize.width + props.gap + unitSize.width);
      const int16_t startX = props.centered ? static_cast<int16_t>(content.x + (content.width - groupW) / 2)
                                            : content.x;
      valueStyle.align = TextAlign::Left;
      drawText(frame.target(), Rect{startX, valueRect.y, valueSize.width, valueRect.height}, props.value, valueStyle);
      drawText(frame.target(),
               Rect{static_cast<int16_t>(startX + valueSize.width + props.gap), valueRect.y, unitSize.width,
                    valueRect.height},
               props.unit, unitStyle);
    } else {
      drawText(frame.target(), valueRect, props.value, valueStyle);
    }
  }
}

template <size_t MaxInteractions>
void tapZones(Frame<MaxInteractions>& frame, Rect rect, const TapZonesProps& props) {
  if (props.zones) {
    for (uint8_t i = 0; i < props.count; ++i) {
      const TapZone& zone = props.zones[i];
      if (!zone.enabled || zone.action == NO_ACTION) continue;
      Rect zoneRect = zone.rect.empty() ? rect : zone.rect;
      frame.hit(zoneRect, zone.action, zone.value, zone.inputMask, zone.state);
    }
  }
  if (props.swipeLeft != NO_ACTION) frame.hit(rect, props.swipeLeft, 0, InputSwipeLeft, StateNormal);
  if (props.swipeRight != NO_ACTION) frame.hit(rect, props.swipeRight, 0, InputSwipeRight, StateNormal);
  if (props.back != NO_ACTION) frame.hit(rect, props.back, 0, InputBack, StateNormal);
}

template <size_t MaxInteractions>
void readerChrome(Frame<MaxInteractions>& frame, Rect rect, const ReaderChromeProps& props) {
  if (props.dimContent) frame.target().fill(rect, Paint::dither(Color::LightGray));
  if (props.showTop && props.topHeight > 0) {
    statusBar(frame, Rect{rect.x, rect.y, rect.width, props.topHeight}, props.top);
  }
  if (props.showBottom && props.bottomHeight > 0) {
    statusBar(frame,
              Rect{rect.x, static_cast<int16_t>(rect.bottom() - props.bottomHeight), rect.width, props.bottomHeight},
              props.bottom);
  }
}

template <size_t MaxInteractions>
void settingRow(Frame<MaxInteractions>& frame, Rect rect, const SettingRowProps& props) {
  State state = props.enabled ? props.state : static_cast<State>(props.state | StateDisabled);
  if (props.action != NO_ACTION && props.enabled) {
    frame.hit(ensureMinTouchRect(rect, props.minTouchSize, frame.screen()), props.action, props.valueId,
              props.inputMask, state);
  }
  state = frame.stateFor(props.action, props.valueId, state);

  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  if (props.radius > 0) setStyleRadius(styles, props.radius);
  const BoxStyle& style = styles.resolve(state);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(Insets{0, props.sidePadding, 0, props.sidePadding});
  const BitmapRef icon = props.icon ? props.icon : resolveBitmap(frame.assets(), props.iconAsset);
  if (icon) {
    const int16_t iconSize = props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon.width);
    Rect iconRect{content.x, static_cast<int16_t>(content.y + (content.height - iconSize) / 2), iconSize, iconSize};
    frame.target().bitmap(iconRect, icon, BitmapMode::Contain, style.foreground);
    content.x = static_cast<int16_t>(content.x + iconSize + props.textGap);
    content.width = static_cast<int16_t>(content.width - iconSize - props.textGap);
  }

  if (props.drawChevron) {
    const int16_t cy = static_cast<int16_t>(content.y + content.height / 2);
    const int16_t x = static_cast<int16_t>(content.right() - 10);
    frame.target().line(Point{x, static_cast<int16_t>(cy - 5)}, Point{static_cast<int16_t>(x + 5), cy}, 2,
                        style.foreground);
    frame.target().line(Point{static_cast<int16_t>(x + 5), cy}, Point{x, static_cast<int16_t>(cy + 5)}, 2,
                        style.foreground);
    content.width = static_cast<int16_t>(content.width - 16);
  }

  if (props.value) {
    TextStyle valueStyle = textStyleWithForeground(props.valueText, style.foreground);
    valueStyle.align = TextAlign::Right;
    const int16_t valueW = frame.target().measureText(valueStyle.font, props.value, valueStyle).width;
    Rect valueRect{static_cast<int16_t>(content.right() - valueW), content.y, valueW, content.height};
    frame.target().text(valueRect, props.value, valueStyle);
    content.width = static_cast<int16_t>(content.width - valueW - props.textGap);
  }

  TextStyle labelStyle = textStyleWithForeground(props.labelText, style.foreground);
  if (props.subtitle) {
    const int16_t labelH = frame.target().lineHeight(labelStyle.font);
    frame.target().text(Rect{content.x, content.y, content.width, labelH}, props.label, labelStyle);
    frame.target().text(Rect{content.x, static_cast<int16_t>(content.y + labelH), content.width,
                             static_cast<int16_t>(content.height - labelH)},
                        props.subtitle, textStyleWithForeground(props.subtitleText, style.foreground));
  } else {
    frame.target().text(content, props.label, labelStyle);
  }
}

template <size_t MaxInteractions>
void toggleRow(Frame<MaxInteractions>& frame, Rect rect, const ToggleRowProps& props) {
  SettingRowProps row = props.row;
  row.value = nullptr;
  if (row.action == NO_ACTION) {
    row.action = props.toggleAction;
    row.valueId = props.toggleValue;
  }
  settingRow(frame, rect, row);

  const int16_t toggleW = props.toggleWidth < 18 ? 18 : props.toggleWidth;
  const int16_t toggleH = props.toggleHeight < 12 ? 12 : props.toggleHeight;
  Rect toggleRect{static_cast<int16_t>(rect.right() - row.sidePadding - toggleW),
                  static_cast<int16_t>(rect.y + (rect.height - toggleH) / 2), toggleW, toggleH};
  const uint8_t trackRadius = static_cast<uint8_t>(
      props.radius > toggleH / 2 ? toggleH / 2 : props.radius);
  frame.target().fill(toggleRect, props.checked ? props.checkedTrack : props.track, trackRadius);
  if (props.border.kind != PaintKind::None && props.borderWidth > 0) {
    frame.target().stroke(toggleRect, props.border, props.borderWidth, trackRadius);
  }

  const int16_t inset = props.knobInset < 0 ? 0 : props.knobInset;
  const int16_t knobH = static_cast<int16_t>(toggleH - inset * 2);
  const int16_t knobW = knobH;
  if (knobH <= 0) return;
  Rect knob{static_cast<int16_t>(props.checked ? toggleRect.right() - inset - knobW : toggleRect.x + inset),
            static_cast<int16_t>(toggleRect.y + inset), knobW, knobH};
  const uint8_t knobRadius = static_cast<uint8_t>(
      props.knobRadius > knobH / 2 ? knobH / 2 : props.knobRadius);
  frame.target().fill(knob, props.checked ? props.checkedKnob : props.knob, knobRadius);
}

template <size_t MaxInteractions>
void stepperRow(Frame<MaxInteractions>& frame, Rect rect, const StepperRowProps& props) {
  const int16_t controlsW =
      static_cast<int16_t>(props.buttonWidth * 2 + props.valueWidth + props.gap * 2);
  SettingRowProps row = props.row;
  row.value = nullptr;
  row.drawChevron = false;
  const int16_t sidePadding = row.sidePadding < 0 ? 0 : row.sidePadding;
  const int16_t controlsX = static_cast<int16_t>(rect.right() - sidePadding - controlsW);
  Rect labelRect = rect;
  labelRect.width = static_cast<int16_t>(controlsX - props.gap - rect.x);
  if (labelRect.width < 0) labelRect.width = 0;
  settingRow(frame, labelRect, row);

  int16_t x = controlsX;
  StyleSet buttonStyles = props.buttonStyles.unset()
                              ? (row.styles.unset() ? defaultButtonStyles() : row.styles)
                              : props.buttonStyles;
  const int16_t visualH = static_cast<int16_t>(
      props.buttonHeight < 18 ? 18 : (props.buttonHeight > rect.height ? rect.height : props.buttonHeight));
  const int16_t visualY = static_cast<int16_t>(rect.y + (rect.height - visualH) / 2);
  auto drawControl = [&](Rect buttonRect, bool plus, ActionId action, int16_t value, Insets hitPadding) {
    ButtonProps buttonProps;
    buttonProps.label = nullptr;
    buttonProps.action = action;
    buttonProps.value = value;
    buttonProps.text = row.valueText;
    buttonProps.styles = buttonStyles;
    buttonProps.minTouchSize = row.minTouchSize;
    buttonProps.radius = props.buttonRadius;
    buttonProps.hitPadding = hitPadding;
    buttonProps.hitPadding.top = static_cast<int16_t>(buttonProps.hitPadding.top + (buttonRect.y - rect.y));
    buttonProps.hitPadding.bottom = static_cast<int16_t>(buttonProps.hitPadding.bottom + (rect.bottom() - buttonRect.bottom()));
    button(frame, buttonRect, buttonProps);

    const int16_t half = static_cast<int16_t>((props.controlSize < 4 ? 4 : props.controlSize) / 2);
    const int16_t cx = static_cast<int16_t>(buttonRect.x + buttonRect.width / 2);
    const int16_t cy = static_cast<int16_t>(buttonRect.y + buttonRect.height / 2);
    const uint8_t stroke = props.controlStroke == 0 ? 1 : props.controlStroke;
    frame.target().line(Point{static_cast<int16_t>(cx - half), cy}, Point{static_cast<int16_t>(cx + half), cy},
                        stroke, props.controlPaint);
    if (plus) {
      frame.target().line(Point{cx, static_cast<int16_t>(cy - half)}, Point{cx, static_cast<int16_t>(cy + half)},
                          stroke, props.controlPaint);
    }
  };

  ButtonProps minus;
  minus.action = props.decrement;
  minus.value = props.decrementValue;
  minus.text = row.valueText;
  minus.styles = buttonStyles;
  minus.minTouchSize = row.minTouchSize;
  minus.hitPadding = Insets{0, static_cast<int16_t>(props.gap / 2), 0, 0};
  drawControl(Rect{x, visualY, props.buttonWidth, visualH}, false, props.decrement, props.decrementValue,
              minus.hitPadding);
  x = static_cast<int16_t>(x + props.buttonWidth + props.gap);

  TextStyle valueText = row.valueText;
  valueText.align = TextAlign::Center;
  Rect valueRect{x, visualY, props.valueWidth, visualH};
  frame.target().text(valueRect, props.value, valueText);
  x = static_cast<int16_t>(x + props.valueWidth + props.gap);

  ButtonProps plus = minus;
  plus.action = props.increment;
  plus.value = props.incrementValue;
  plus.hitPadding = Insets{0, 0, 0, static_cast<int16_t>(props.gap / 2)};
  drawControl(Rect{x, visualY, props.buttonWidth, visualH}, true, props.increment, props.incrementValue,
              plus.hitPadding);
}

template <size_t MaxInteractions>
void radioGroup(Frame<MaxInteractions>& frame, Rect rect, const RadioGroupProps& props) {
  if (!props.options || props.count == 0) return;
  const int16_t totalGap = static_cast<int16_t>((props.count - 1) * props.gap);
  const int16_t cellW = static_cast<int16_t>((rect.width - totalGap) / props.count);
  int16_t x = rect.x;
  for (uint8_t i = 0; i < props.count; ++i) {
    const RadioOption& option = props.options[i];
    Rect cell{x, rect.y, i == props.count - 1 ? static_cast<int16_t>(rect.right() - x) : cellW, rect.height};
    ButtonProps buttonProps;
    buttonProps.label = option.label;
    buttonProps.action = props.action;
    buttonProps.value = option.value;
    buttonProps.inputMask = props.inputMask;
    buttonProps.state = option.value == props.selectedValue ? StateSelected : StateNormal;
    buttonProps.text = props.text;
    buttonProps.styles = props.styles;
    buttonProps.minTouchSize = props.minTouchSize;
    buttonProps.radius = props.radius;
    buttonProps.enabled = option.enabled;
    button(frame, cell, buttonProps);
    x = static_cast<int16_t>(x + cell.width + props.gap);
  }
}

template <size_t MaxInteractions>
void contextMenu(Frame<MaxInteractions>& frame, Rect rect, const ContextMenuProps& props) {
  if (props.dimBackground) frame.target().fill(frame.screen(), Paint::dither(Color::LightGray));
  StyleSet panelStyles = props.panelStyles.unset() ? defaultPopupStyles() : props.panelStyles;
  const BoxStyle& panel = panelStyles.resolve(StateNormal);
  frame.target().fill(rect, panel.background, panel.radius, panel.corners);
  if (panel.border.kind != PaintKind::None && panel.borderWidth > 0) {
    frame.target().stroke(rect, panel.border, panel.borderWidth, panel.radius, panel.corners);
  }

  Rect content = rect.inset(props.padding);
  if (props.title) {
    const int16_t titleH = frame.target().lineHeight(props.titleText.font);
    frame.target().text(Rect{content.x, content.y, content.width, titleH}, props.title, props.titleText);
    content.y = static_cast<int16_t>(content.y + titleH + props.gap);
    content.height = static_cast<int16_t>(content.height - titleH - props.gap);
  }

  for (uint8_t i = 0; props.options && i < props.optionCount; ++i) {
    Rect row{content.x, static_cast<int16_t>(content.y + i * (props.rowHeight + props.gap)), content.width,
             props.rowHeight};
    if (row.bottom() > content.bottom()) break;
    ButtonProps bp;
    bp.label = props.options[i].label;
    bp.action = props.options[i].action;
    bp.value = props.options[i].value;
    bp.inputMask = props.inputMask;
    bp.state = props.options[i].state;
    bp.enabled = props.options[i].enabled;
    bp.text = props.itemText;
    bp.styles = props.itemStyles;
    bp.minTouchSize = frame.device().minTouchSize;
    button(frame, row, bp);
  }
}

template <size_t MaxInteractions>
void toast(Frame<MaxInteractions>& frame, Rect bounds, const ToastProps& props) {
  if (!props.message) return;
  const int16_t maxW = props.maxWidth > 0 ? props.maxWidth : static_cast<int16_t>(bounds.width * 3 / 4);
  TextStyle text = props.text;
  text.align = TextAlign::Center;
  const Size textSize = measureWrappedText(frame.target(), props.message, text,
                                           static_cast<int16_t>(maxW - props.padding.left - props.padding.right));
  const Size panelSize{static_cast<int16_t>(textSize.width + props.padding.left + props.padding.right),
                       static_cast<int16_t>(textSize.height + props.padding.top + props.padding.bottom)};
  int16_t y = bounds.y;
  if (props.anchor == ToastAnchor::Center) {
    y = static_cast<int16_t>(bounds.y + (bounds.height - panelSize.height) / 2);
  } else if (props.anchor == ToastAnchor::Bottom) {
    y = static_cast<int16_t>(bounds.bottom() - panelSize.height - props.margin);
  } else {
    y = static_cast<int16_t>(bounds.y + props.margin);
  }
  Rect panel{static_cast<int16_t>(bounds.x + (bounds.width - panelSize.width) / 2), y, panelSize.width,
             panelSize.height};
  PopupProps popupProps;
  popupProps.message = props.message;
  popupProps.text = text;
  popupProps.styles = props.styles;
  popupProps.padding = props.padding;
  popup(frame, panel, popupProps);
}

template <size_t MaxInteractions>
void messagePanel(Frame<MaxInteractions>& frame, Rect rect, const MessagePanelProps& props) {
  StyleSet styles = props.panelStyles.unset() ? defaultPopupStyles() : props.panelStyles;
  const BoxStyle& style = styles.resolve(StateNormal);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  int16_t buttonH = props.actionLabel ? props.buttonHeight : 0;
  int16_t progressH = props.showProgress ? 4 : 0;
  int16_t y = content.y;
  if (props.title) {
    const int16_t titleH = frame.target().lineHeight(props.titleText.font);
    TextStyle title = props.titleText;
    title.align = TextAlign::Center;
    frame.target().text(Rect{content.x, y, content.width, titleH}, props.title, title);
    y = static_cast<int16_t>(y + titleH + props.gap);
  }
  if (props.message) {
    TextStyle msg = props.messageText;
    msg.align = TextAlign::Center;
    Rect msgRect{content.x, y, content.width,
                 static_cast<int16_t>(content.bottom() - y - buttonH - progressH - props.gap * 2)};
    frame.target().text(msgRect, props.message, msg);
  }
  if (props.showProgress) {
    progressBar(frame, Rect{content.x, static_cast<int16_t>(content.bottom() - buttonH - props.gap - 4),
                            content.width, 4},
                props.progress);
  }
  if (props.actionLabel) {
    ButtonProps bp;
    bp.label = props.actionLabel;
    bp.action = props.action;
    bp.value = props.actionValue;
    bp.text = props.buttonText;
    bp.styles = props.buttonStyles;
    bp.minTouchSize = frame.device().minTouchSize;
    button(frame, Rect{content.x, static_cast<int16_t>(content.bottom() - buttonH), content.width, buttonH}, bp);
  }
}

template <size_t MaxInteractions>
void bookCard(Frame<MaxInteractions>& frame, Rect rect, const BookCardProps& props) {
  State state = props.enabled ? props.state : static_cast<State>(props.state | StateDisabled);
  if (props.action != NO_ACTION && props.enabled) {
    frame.hit(ensureMinTouchRect(rect, frame.device().minTouchSize, frame.screen()), props.action, props.value,
              InputDefault, state);
  }
  state = frame.stateFor(props.action, props.value, state);
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  const BoxStyle& style = styles.resolve(state);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  const BitmapRef cover = props.cover ? props.cover : resolveBitmap(frame.assets(), props.coverAsset);
  const int16_t coverW = props.coverSize.width > content.width ? content.width : props.coverSize.width;
  const int16_t coverH = props.coverSize.height > content.height ? content.height : props.coverSize.height;
  Rect coverRect{content.x, static_cast<int16_t>(content.y + (content.height - coverH) / 2), coverW, coverH};
  frame.target().fill(coverRect, Paint::dither(Color::LightGray));
  if (cover) frame.target().bitmap(coverRect, cover, BitmapMode::Cover, style.foreground);
  content.x = static_cast<int16_t>(coverRect.right() + props.gap);
  content.width = static_cast<int16_t>(rect.right() - props.padding.right - content.x);
  const int16_t progressH = props.progressHeight < 1 ? 1 : props.progressHeight;
  const int16_t progressY = static_cast<int16_t>(coverRect.bottom() - progressH);
  const int16_t textBottom = static_cast<int16_t>(progressY - props.textProgressGap);

  int16_t y = content.y;
  if (props.title) {
    TextStyle title = textStyleWithForeground(props.titleText, style.foreground);
    title.maxLines = title.maxLines ? title.maxLines : 2;
    const int16_t h = measureWrappedText(frame.target(), props.title, title, content.width).height;
    frame.target().text(Rect{content.x, y, content.width, h}, props.title, title);
    y = static_cast<int16_t>(y + h + props.textGap);
  }
  if (props.author && y < textBottom) {
    const int16_t h = frame.target().lineHeight(props.authorText.font);
    frame.target().text(Rect{content.x, y, content.width, h}, props.author,
                        textStyleWithForeground(props.authorText, style.foreground));
    y = static_cast<int16_t>(y + h + props.textGap);
  }
  if (props.meta && y < textBottom) {
    const int16_t h = frame.target().lineHeight(props.metaText.font);
    frame.target().text(Rect{content.x, y, content.width, h}, props.meta,
                        textStyleWithForeground(props.metaText, style.foreground));
  }
  if (props.progressMax > 0) {
    ProgressBarProps progress;
    progress.value = props.progress;
    progress.max = props.progressMax;
    progress.track = Paint::dither(Color::LightGray);
    progress.fill = Paint::solid(Color::Black);
    progressBar(frame, Rect{content.x, progressY, content.width, progressH}, progress);
  }
}

template <size_t MaxInteractions>
void coverGrid(Frame<MaxInteractions>& frame, Rect rect, const CoverGridProps& props) {
  if (!props.items || props.count == 0 || props.columns == 0) return;
  const int16_t cellW = static_cast<int16_t>((rect.width - (props.columns - 1) * props.gap) / props.columns);
  const int16_t strideY = static_cast<int16_t>(props.rowHeight + props.gap);
  const uint16_t rowsVisible = props.rowHeight > 0 ? static_cast<uint16_t>((rect.height + props.gap) / strideY) : 0;
  const uint16_t cellsVisible = static_cast<uint16_t>(rowsVisible * props.columns);
  uint16_t top = props.topIndex;
  if (top >= props.count) top = 0;
  for (uint16_t visible = 0; visible < cellsVisible && top + visible < props.count; ++visible) {
    const uint16_t index = static_cast<uint16_t>(top + visible);
    const uint8_t col = static_cast<uint8_t>(visible % props.columns);
    const uint16_t row = static_cast<uint16_t>(visible / props.columns);
    Rect cell{static_cast<int16_t>(rect.x + col * (cellW + props.gap)),
              static_cast<int16_t>(rect.y + row * strideY), cellW, props.rowHeight};
    const CoverGridItem& item = props.items[index];
    State state = item.state;
    if (props.selectedIndex == static_cast<int16_t>(index)) state |= StateSelected;
    if (!item.enabled) state |= StateDisabled;
    if (props.action != NO_ACTION && item.enabled) {
      frame.hit(ensureMinTouchRect(cell, props.minTouchSize, frame.screen()), props.action, item.actionValue,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, item.actionValue, state);
    StyleSet styles = props.cellStyles.unset() ? defaultListRowStyles() : props.cellStyles;
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(cell, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(cell, style.border, style.borderWidth, style.radius, style.corners);
    }
    Rect coverRect{static_cast<int16_t>(cell.x + (cell.width - props.coverSize.width) / 2), cell.y,
                   props.coverSize.width, props.coverSize.height};
    frame.target().fill(coverRect, Paint::dither(Color::LightGray));
    const BitmapRef cover = item.cover ? item.cover : resolveBitmap(frame.assets(), item.coverAsset);
    if (cover) frame.target().bitmap(coverRect, cover, BitmapMode::Cover, style.foreground);
    if (item.title && props.labelHeight > 0) {
      TextStyle title = textStyleWithForeground(props.titleText, style.foreground);
      title.align = TextAlign::Center;
      title.maxLines = title.maxLines > 0 ? title.maxLines : 1;
      frame.target().text(Rect{cell.x, static_cast<int16_t>(coverRect.bottom() + 2), cell.width, props.labelHeight},
                          item.title, title);
    }
  }
}

template <size_t MaxInteractions>
void optionDialog(Frame<MaxInteractions>& frame, Rect rect, const OptionDialogProps& props) {
  if (props.dimBackground) {
    frame.target().fill(frame.screen(), Paint::dither(Color::LightGray));
  }
  StyleSet styles = props.styles.unset() ? defaultPopupStyles() : props.styles;
  const BoxStyle& style = styles.resolve(StateNormal);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  int16_t cursorY = content.y;
  if (props.title) {
    const int16_t lh = frame.target().lineHeight(props.titleText.font);
    drawText(frame.target(), Rect{content.x, cursorY, content.width, lh}, props.title, props.titleText);
    cursorY = static_cast<int16_t>(cursorY + lh + props.gap);
  }

  if (props.headline) {
    // Wrap and draw in one layoutText pass so the reserved height always
    // matches the rendered lines; the height-1 rect top-anchors the block at
    // the cursor (vertical centering only engages when the rect is taller
    // than the text).
    const int16_t lh = frame.target().lineHeight(props.headlineText.font);
    TextStyle lineStyle = props.headlineText;
    lineStyle.align = TextAlign::Left;  // runs arrive already positioned
    lineStyle.maxLines = 1;
    int16_t lines = 0;
    layoutText(frame.target(), Rect{content.x, cursorY, content.width, 1}, props.headline, props.headlineText,
               [&](const char* line, Rect r) {
                 frame.target().text(r, line, lineStyle);
                 ++lines;
               });
    cursorY = static_cast<int16_t>(cursorY + lines * lh + props.gap);
  }

  int16_t buttonsH = 0;
  if (props.options && props.optionCount > 0) {
    buttonsH = props.verticalOptions
                   ? static_cast<int16_t>(props.optionCount * props.buttonHeight +
                                          (props.optionCount - 1) * props.gap)
                   : props.buttonHeight;
  }

  if (props.message) {
    Rect messageRect{content.x, cursorY, content.width,
                     static_cast<int16_t>(content.bottom() - buttonsH - (buttonsH ? props.gap : 0) - cursorY)};
    drawText(frame.target(), messageRect, props.message, props.messageText);
  }

  if (!props.options || props.optionCount == 0) return;
  Rect buttons{content.x, static_cast<int16_t>(content.bottom() - buttonsH), content.width, buttonsH};
  for (uint8_t i = 0; i < props.optionCount; ++i) {
    const DialogOption& option = props.options[i];
    Rect optionRect;
    if (props.verticalOptions) {
      optionRect = Rect{buttons.x, static_cast<int16_t>(buttons.y + i * (props.buttonHeight + props.gap)),
                        buttons.width, props.buttonHeight};
    } else {
      const int16_t w =
          static_cast<int16_t>((buttons.width - (props.optionCount - 1) * props.gap) / props.optionCount);
      optionRect = Rect{static_cast<int16_t>(buttons.x + i * (w + props.gap)), buttons.y, w, buttons.height};
    }
    ButtonProps bp;
    bp.label = option.label;
    bp.action = option.action;
    bp.value = option.value;
    bp.inputMask = props.inputMask;
    bp.state = option.state;
    bp.text = props.buttonText;
    bp.styles = props.buttonStyles;
    bp.minTouchSize = frame.device().minTouchSize;
    bp.enabled = option.enabled;
    button(frame, optionRect, bp);
  }
}

}  // namespace ui
}  // namespace freeink
