#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

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

}  // namespace ui
}  // namespace freeink
