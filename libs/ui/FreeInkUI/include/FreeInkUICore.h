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
  bool explicitlySet = false;

  // True when the caller never assigned any visible style, so components fall
  // back to their built-in defaults. Borders count too: an outline-only style
  // (transparent background) is still "set".
  bool unset() const {
    return !explicitlySet && normal.background.kind == PaintKind::None && normal.border.kind == PaintKind::None &&
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


StyleSet defaultButtonStyles();
StyleSet defaultListRowStyles();
StyleSet defaultKeyStyles();
StyleSet defaultPopupStyles();
StyleSet plainStyles(Paint foreground = Paint::solid(Color::Black));
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

}  // namespace ui
}  // namespace freeink
