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
  int16_t textGap = 10;
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

  // Slot layout: the label owns a "title band" and every accessory — icon,
  // chevron, value — aligns to that band, not the row's vertical middle. With
  // a subtitle the label+subtitle block centers in the row and the subtitle
  // spans the full content width under the accessories, so it never collides
  // with them.
  TextStyle labelStyle = textStyleWithForeground(props.labelText, style.foreground);
  const int16_t labelH = frame.target().lineHeight(labelStyle.font);
  const int16_t subH = props.subtitle ? frame.target().lineHeight(props.subtitleText.font) : 0;
  Rect band = content;
  if (props.subtitle) {
    int16_t top = static_cast<int16_t>(content.y + (content.height - labelH - subH) / 2);
    if (top < content.y) top = content.y;
    band = Rect{content.x, top, content.width, labelH};
  }

  const BitmapRef icon = props.icon ? props.icon : resolveBitmap(frame.assets(), props.iconAsset);
  if (icon) {
    const int16_t iconSize = props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon.width);
    Rect iconRect{content.x, static_cast<int16_t>(band.y + (band.height - iconSize) / 2), iconSize, iconSize};
    frame.target().bitmap(iconRect, icon, BitmapMode::Contain, style.foreground);
    content.x = static_cast<int16_t>(content.x + iconSize + props.textGap);
    content.width = static_cast<int16_t>(content.width - iconSize - props.textGap);
    band.x = content.x;
    band.width = content.width;
  }

  int16_t availW = band.width;   // label width; shrinks as accessories claim the right edge
  if (props.drawChevron) {
    const int16_t cy = static_cast<int16_t>(band.y + band.height / 2);
    const int16_t x = static_cast<int16_t>(content.right() - 10);
    frame.target().line(Point{x, static_cast<int16_t>(cy - 5)}, Point{static_cast<int16_t>(x + 5), cy}, 2,
                        style.foreground);
    frame.target().line(Point{static_cast<int16_t>(x + 5), cy}, Point{x, static_cast<int16_t>(cy + 5)}, 2,
                        style.foreground);
    availW = static_cast<int16_t>(availW - 16);
  }

  if (props.value) {
    TextStyle valueStyle = textStyleWithForeground(props.valueText, style.foreground);
    valueStyle.align = TextAlign::Right;
    const int16_t valueW = frame.target().measureText(valueStyle.font, props.value, valueStyle).width;
    Rect valueRect{static_cast<int16_t>(band.x + availW - valueW), band.y, valueW, band.height};
    frame.target().text(valueRect, props.value, valueStyle);
    availW = static_cast<int16_t>(availW - valueW - props.textGap);
  }

  frame.target().text(Rect{band.x, band.y, availW, band.height}, props.label, labelStyle);
  if (props.subtitle) {
    frame.target().text(Rect{content.x, static_cast<int16_t>(band.y + labelH), content.width, subH},
                        props.subtitle, textStyleWithForeground(props.subtitleText, style.foreground));
  }
}

}  // namespace ui
}  // namespace freeink
