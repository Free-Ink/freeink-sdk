#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

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

}  // namespace ui
}  // namespace freeink
