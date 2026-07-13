#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct DropdownProps {
  const char* label = nullptr;
  const char* value = nullptr;
  // With a subtitle the row renders settingRow-style: label on top,
  // subtitle (typically the current selection) beneath in subtitleText.
  // `value` is ignored in that layout.
  const char* subtitle = nullptr;
  BitmapRef icon{};        // optional leading icon (like settingRow)
  AssetRef iconAsset{};
  int16_t iconSize = 0;    // 0 = icon's natural size
  ActionId action = NO_ACTION;
  int16_t actionValue = 0;
  uint16_t inputMask = InputDefault;
  TextStyle labelText{};
  TextStyle valueText{};
  TextStyle subtitleText{};
  StyleSet styles{};
  State state = StateNormal;
  Insets padding{6, 8, 6, 8};
  int16_t gap = 8;
  int16_t titleSubtitleGap = 2;
  int16_t indicatorWidth = 16;
  int16_t indicatorSize = 10;
  uint8_t indicatorStroke = 2;
  int16_t minTouchSize = 44;
  uint8_t radius = 0;
  bool enabled = true;
};

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
  TextStyle labelStyle = textStyleWithForeground(props.labelText, style.foreground);
  TextStyle subtitleStyle = textStyleWithForeground(props.subtitleText, style.foreground);
  const int16_t labelH = frame.target().lineHeight(labelStyle.font);
  const int16_t subH = props.subtitle ? frame.target().lineHeight(subtitleStyle.font) : 0;
  const int16_t titleSubGap = props.subtitle ? (props.titleSubtitleGap > 0 ? props.titleSubtitleGap : 0) : 0;
  Rect band = content;
  if (props.label && props.subtitle) {
    int16_t top = static_cast<int16_t>(content.y + (content.height - labelH - titleSubGap - subH) / 2);
    if (top < content.y) top = content.y;
    band = Rect{content.x, top, content.width, labelH};
  }
  const BitmapRef icon = props.icon ? props.icon : resolveBitmap(frame.assets(), props.iconAsset);
  if (icon) {
    const int16_t iconSize = props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon.width);
    Rect iconRect{content.x, static_cast<int16_t>(band.y + (band.height - iconSize) / 2),
                  iconSize, iconSize};
    const BoxStyle& iconStyle = (props.styles.unset() ? defaultButtonStyles() : props.styles).resolve(state);
    frame.target().bitmap(iconRect, icon, BitmapMode::Contain, iconStyle.foreground);
    const int16_t shift = static_cast<int16_t>(iconSize + props.gap);
    content.x = static_cast<int16_t>(content.x + shift);
    content.width = static_cast<int16_t>(content.width - shift);
  }
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
  if (props.label && props.subtitle) {
    // Two-line layout: label above, subtitle (current selection) below.
    Rect labelRect{textRect.x, band.y, textRect.width, labelH};
    frame.target().text(labelRect, props.label, textStyleWithForeground(labelStyle, ink));
    Rect subRect{textRect.x, static_cast<int16_t>(band.y + labelH + titleSubGap), textRect.width, subH};
    frame.target().text(subRect, props.subtitle, textStyleWithForeground(subtitleStyle, ink));
  } else if (props.label && props.value) {
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

}  // namespace ui
}  // namespace freeink
