#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

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

}  // namespace ui
}  // namespace freeink
