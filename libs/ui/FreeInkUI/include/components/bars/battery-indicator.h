#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

enum class BatteryIndicatorStyle : uint8_t {
  Icon,
  Bar,
};

enum class BatteryBarTrack : uint8_t {
  None,
  Hairline,
  Outline,
  Dither,
};

enum class BatteryBarFill : uint8_t {
  Solid,
  Dither,
  Segments,
};

enum class BatteryBarDirection : uint8_t {
  LeftToRight,
  RightToLeft,
  CenterOut,
  BottomToTop,
  TopToBottom,
};

enum class BatteryBarCaps : uint8_t {
  Square,
  Pixel,
};

enum class BatteryBarOrientation : uint8_t {
  Horizontal,
  Vertical,
};

struct BatteryIndicatorProps {
  uint8_t percent = 0; // 0..100
  bool charging = false;
  BatteryIndicatorStyle style = BatteryIndicatorStyle::Icon;
  BatteryBarTrack barTrack = BatteryBarTrack::None;
  BatteryBarFill barFill = BatteryBarFill::Solid;
  BatteryBarDirection barDirection = BatteryBarDirection::LeftToRight;
  BatteryBarCaps barCaps = BatteryBarCaps::Square;
  BatteryBarOrientation barOrientation = BatteryBarOrientation::Horizontal;
  uint8_t barSegments = 0;
  int16_t barSegmentGap = 1;
  uint8_t barRadius = 0;
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

template <size_t MaxInteractions>
void batteryIndicator(Frame<MaxInteractions>& frame, Rect rect,
                      const BatteryIndicatorProps& props) {
  const bool isBar = props.style == BatteryIndicatorStyle::Bar;
  const int16_t minGlyphH = isBar ? 1 : 6;
  const int16_t minGlyphW = isBar ? 1 : 8;
  const int16_t glyphH =
      props.glyphHeight < minGlyphH ? minGlyphH : props.glyphHeight;
  const int16_t glyphW =
      props.glyphWidth < minGlyphW ? minGlyphW : props.glyphWidth;
  const int16_t nubW = props.style == BatteryIndicatorStyle::Icon ? 2 : 0;
  const int16_t nubH = static_cast<int16_t>(glyphH / 2);
  Rect body{static_cast<int16_t>(rect.right() - glyphW - nubW),
            static_cast<int16_t>(rect.y + (rect.height - glyphH) / 2), glyphW,
            glyphH};
  const Paint ink = Paint::solid(props.color);

  const uint8_t percent = props.percent > 100 ? 100 : props.percent;
  Rect cavity = props.style == BatteryIndicatorStyle::Bar
                    ? body
                    : body.inset(Insets{2, 2, 2, 2});
  if (props.style == BatteryIndicatorStyle::Icon) {
    frame.target().stroke(body, ink, 1);
    frame.target().fill(Rect{body.right(),
                             static_cast<int16_t>(body.y + (glyphH - nubH) / 2),
                             nubW, nubH},
                        ink);
  }
  if (props.style == BatteryIndicatorStyle::Bar) {
    if (props.barTrack == BatteryBarTrack::Dither) {
      frame.target().fill(body, Paint::dither(Color::LightGray), props.barRadius);
    } else if (props.barTrack == BatteryBarTrack::Outline) {
      frame.target().stroke(body, ink, 1, props.barRadius);
      if (body.width > 2 && body.height > 2) {
        cavity = body.inset(Insets{1, 1, 1, 1});
      }
    } else if (props.barTrack == BatteryBarTrack::Hairline) {
      if (props.barOrientation == BatteryBarOrientation::Vertical) {
        const int16_t x = static_cast<int16_t>(body.x + body.width / 2);
        frame.target().line(Point{x, body.y}, Point{x, static_cast<int16_t>(body.bottom() - 1)}, 1, ink);
      } else {
        const int16_t y = static_cast<int16_t>(body.y + body.height / 2);
        frame.target().line(Point{body.x, y}, Point{static_cast<int16_t>(body.right() - 1), y}, 1, ink);
      }
    }

    const Paint fillPaint = props.barFill == BatteryBarFill::Dither ? Paint::dither(props.color) : ink;
    auto softenCaps = [&](Rect r) {
      if (props.barCaps != BatteryBarCaps::Pixel || r.width < 3 || r.height < 3) return;
      const Paint paper = Paint::solid(invertedColor(props.color));
      frame.target().fill(Rect{r.x, r.y, 1, 1}, paper);
      frame.target().fill(Rect{static_cast<int16_t>(r.right() - 1), r.y, 1, 1}, paper);
      frame.target().fill(Rect{r.x, static_cast<int16_t>(r.bottom() - 1), 1, 1}, paper);
      frame.target().fill(Rect{static_cast<int16_t>(r.right() - 1), static_cast<int16_t>(r.bottom() - 1), 1, 1}, paper);
    };
    auto fillRect = [&](Rect r) {
      if (r.width <= 0 || r.height <= 0) return;
      frame.target().fill(r, fillPaint, props.barRadius);
      softenCaps(r);
    };

    const bool vertical = props.barOrientation == BatteryBarOrientation::Vertical ||
                          props.barDirection == BatteryBarDirection::BottomToTop ||
                          props.barDirection == BatteryBarDirection::TopToBottom;
    if (props.barFill == BatteryBarFill::Segments && props.barSegments > 1) {
      const int16_t count = static_cast<int16_t>(props.barSegments);
      const int16_t gap = props.barSegmentGap < 0 ? 0 : props.barSegmentGap;
      const int16_t filled = static_cast<int16_t>((static_cast<int32_t>(percent) * count + 99) / 100);
      if (vertical) {
        const int16_t totalGap = static_cast<int16_t>(gap * (count - 1));
        const int16_t segH = static_cast<int16_t>((cavity.height - totalGap) / count);
        for (int16_t i = 0; i < count && segH > 0; ++i) {
          const int16_t drawIndex =
              props.barDirection == BatteryBarDirection::TopToBottom ? i : static_cast<int16_t>(count - 1 - i);
          const bool on = i < filled;
          if (on) {
            fillRect(Rect{cavity.x, static_cast<int16_t>(cavity.y + drawIndex * (segH + gap)), cavity.width, segH});
          }
        }
      } else {
        const int16_t totalGap = static_cast<int16_t>(gap * (count - 1));
        const int16_t segW = static_cast<int16_t>((cavity.width - totalGap) / count);
        for (int16_t i = 0; i < count && segW > 0; ++i) {
          const int16_t drawIndex =
              props.barDirection == BatteryBarDirection::RightToLeft ? static_cast<int16_t>(count - 1 - i) : i;
          const bool on = i < filled;
          if (on) {
            fillRect(Rect{static_cast<int16_t>(cavity.x + drawIndex * (segW + gap)), cavity.y, segW, cavity.height});
          }
        }
      }
    } else if (vertical) {
      const int16_t fillH = static_cast<int16_t>((static_cast<int32_t>(cavity.height) * percent) / 100);
      if (fillH > 0) {
        const int16_t y = props.barDirection == BatteryBarDirection::TopToBottom
                              ? cavity.y
                              : static_cast<int16_t>(cavity.bottom() - fillH);
        fillRect(Rect{cavity.x, y, cavity.width, fillH});
      }
    } else {
      const int16_t fillW = static_cast<int16_t>((static_cast<int32_t>(cavity.width) * percent) / 100);
      if (fillW > 0) {
        int16_t x = cavity.x;
        if (props.barDirection == BatteryBarDirection::RightToLeft) {
          x = static_cast<int16_t>(cavity.right() - fillW);
        } else if (props.barDirection == BatteryBarDirection::CenterOut) {
          x = static_cast<int16_t>(cavity.x + (cavity.width - fillW) / 2);
        }
        fillRect(Rect{x, cavity.y, fillW, cavity.height});
      }
    }
  } else {
    const int16_t fillW = static_cast<int16_t>(
        (static_cast<int32_t>(cavity.width) * percent) / 100);
    if (fillW > 0) {
      frame.target().fill(Rect{cavity.x, cavity.y, fillW, cavity.height}, ink);
    }
  }
  if (props.charging) {
    if (props.chargingIcon) {
      frame.target().bitmap(
          centeredRect(body,
                       Size{static_cast<int16_t>(props.chargingIcon.width),
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
      frame.target().triangle(
          Point{static_cast<int16_t>(cx + w / 2),
                static_cast<int16_t>(cy - h / 2)},
          Point{static_cast<int16_t>(cx - w), static_cast<int16_t>(cy + 1)},
          Point{static_cast<int16_t>(cx + 1), static_cast<int16_t>(cy + 1)},
          bolt);
      frame.target().triangle(
          Point{static_cast<int16_t>(cx - w / 2),
                static_cast<int16_t>(cy + h / 2)},
          Point{static_cast<int16_t>(cx + w), static_cast<int16_t>(cy - 1)},
          Point{static_cast<int16_t>(cx - 1), static_cast<int16_t>(cy - 1)},
          bolt);
    }
  }
  if (props.label) {
    TextStyle style = props.text;
    style.align = TextAlign::Right;
    Rect labelRect{rect.x, rect.y,
                   static_cast<int16_t>(body.x - props.gap - rect.x),
                   rect.height};
    drawText(frame.target(), labelRect, props.label, style);
  }
}

}  // namespace ui
}  // namespace freeink
