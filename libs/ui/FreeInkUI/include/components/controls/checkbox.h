#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

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

}  // namespace ui
}  // namespace freeink
