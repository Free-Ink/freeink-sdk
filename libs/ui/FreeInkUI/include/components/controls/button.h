#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

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

}  // namespace ui
}  // namespace freeink
