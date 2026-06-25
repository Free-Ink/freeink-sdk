#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/button.h"

namespace freeink {
namespace ui {

struct RadioOption {
  const char* label = nullptr;
  int16_t value = 0;
  bool enabled = true;
};

struct RadioGroupProps {
  const RadioOption* options = nullptr;
  uint8_t count = 0;
  int16_t selectedValue = 0;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle text{};
  StyleSet styles{};
  int16_t gap = 4;
  int16_t minTouchSize = 44;
  uint8_t radius = 0;
};

template <size_t MaxInteractions>
void radioGroup(Frame<MaxInteractions>& frame, Rect rect, const RadioGroupProps& props) {
  if (!props.options || props.count == 0) return;
  const int16_t totalGap = static_cast<int16_t>((props.count - 1) * props.gap);
  const int16_t cellW = static_cast<int16_t>((rect.width - totalGap) / props.count);
  int16_t x = rect.x;
  for (uint8_t i = 0; i < props.count; ++i) {
    const RadioOption& option = props.options[i];
    Rect cell{x, rect.y, i == props.count - 1 ? static_cast<int16_t>(rect.right() - x) : cellW, rect.height};
    ButtonProps buttonProps;
    buttonProps.label = option.label;
    buttonProps.action = props.action;
    buttonProps.value = option.value;
    buttonProps.inputMask = props.inputMask;
    buttonProps.state = option.value == props.selectedValue ? StateSelected : StateNormal;
    buttonProps.text = props.text;
    buttonProps.styles = props.styles;
    buttonProps.minTouchSize = props.minTouchSize;
    buttonProps.radius = props.radius;
    buttonProps.enabled = option.enabled;
    button(frame, cell, buttonProps);
    x = static_cast<int16_t>(x + cell.width + props.gap);
  }
}

}  // namespace ui
}  // namespace freeink
