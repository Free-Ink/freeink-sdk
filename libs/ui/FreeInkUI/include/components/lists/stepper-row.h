#pragma once

#include "../../FreeInkUICore.h"
#include "../lists/setting-row.h"
#include "../controls/button.h"

namespace freeink {
namespace ui {

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

}  // namespace ui
}  // namespace freeink
