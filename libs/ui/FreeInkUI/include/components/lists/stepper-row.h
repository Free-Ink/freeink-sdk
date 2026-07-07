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
  // Control sizes: 0 (the default) derives them from the value font's line
  // height, so the stepper fits whatever font the target binds instead of
  // assuming a small UI font. Set explicit pixels to override.
  int16_t buttonWidth = 0;
  int16_t buttonHeight = 0;
  int16_t valueWidth = 0;
  // Widest value string this stepper will show (e.g. "240 min"). The derived
  // valueWidth measures it so the controls don't shift as the value changes;
  // unset, the current value is measured instead.
  const char* widestValue = nullptr;
  int16_t gap = 6;
  StyleSet buttonStyles{};
  Paint controlPaint = Paint::solid(Color::Black);
  int16_t controlSize = 0;  // 0 = derive from the value font's line height
  uint8_t controlStroke = 2;
  uint8_t buttonRadius = 0;
};

template <size_t MaxInteractions>
void stepperRow(Frame<MaxInteractions>& frame, Rect rect, const StepperRowProps& props) {
  const int16_t lh = frame.target().lineHeight(props.row.valueText.font);
  const int16_t buttonHeight = props.buttonHeight > 0 ? props.buttonHeight : static_cast<int16_t>(lh + 10);
  const int16_t buttonWidth = props.buttonWidth > 0 ? props.buttonWidth : static_cast<int16_t>(buttonHeight + 8);
  int16_t valueWidth = props.valueWidth;
  if (valueWidth <= 0) {
    const char* widest = props.widestValue ? props.widestValue : props.value;
    valueWidth = static_cast<int16_t>(
        (widest ? frame.target().measureText(props.row.valueText.font, widest, props.row.valueText).width : lh) + 12);
  }
  const int16_t controlsW = static_cast<int16_t>(buttonWidth * 2 + valueWidth + props.gap * 2);
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
      buttonHeight < 18 ? 18 : (buttonHeight > rect.height ? rect.height : buttonHeight));
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

    const int16_t controlSize = props.controlSize > 0 ? props.controlSize : static_cast<int16_t>(lh / 3 + 2);
    const int16_t half = static_cast<int16_t>((controlSize < 4 ? 4 : controlSize) / 2);
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
  drawControl(Rect{x, visualY, buttonWidth, visualH}, false, props.decrement, props.decrementValue,
              minus.hitPadding);
  x = static_cast<int16_t>(x + buttonWidth + props.gap);

  TextStyle valueText = row.valueText;
  valueText.align = TextAlign::Center;
  Rect valueRect{x, visualY, valueWidth, visualH};
  frame.target().text(valueRect, props.value, valueText);
  x = static_cast<int16_t>(x + valueWidth + props.gap);

  ButtonProps plus = minus;
  plus.action = props.increment;
  plus.value = props.incrementValue;
  plus.hitPadding = Insets{0, 0, 0, static_cast<int16_t>(props.gap / 2)};
  drawControl(Rect{x, visualY, buttonWidth, visualH}, true, props.increment, props.incrementValue,
              plus.hitPadding);
}

}  // namespace ui
}  // namespace freeink
