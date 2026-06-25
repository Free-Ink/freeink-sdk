#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/button.h"

namespace freeink {
namespace ui {

struct GestureBarButton {
  const char* label = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  ActionId action = NO_ACTION;
  int16_t value = 0;
  State state = StateNormal;
};

struct GestureBarProps {
  GestureBarButton left{};
  GestureBarButton center{};
  GestureBarButton right{};
  ActionId swipeLeft = NO_ACTION;
  ActionId swipeRight = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  int16_t height = 44;
  int16_t gap = 0;
  TextStyle text{};
  StyleSet styles{};
};

template <size_t MaxInteractions, size_t MaxSlots = 3>
void gestureBar(Frame<MaxInteractions>& frame, Rect rect, const GestureBarProps& props) {
  Stack<MaxSlots> row(rect, Axis::Row, props.gap);
  row.flex(1);
  row.flex(1);
  row.flex(1);
  row.layout();
  const GestureBarButton buttons[3] = {props.left, props.center, props.right};
  for (uint8_t i = 0; i < 3; ++i) {
    const GestureBarButton& item = buttons[i];
    if (item.action == NO_ACTION && !item.label && !item.icon) continue;
    ButtonProps bp;
    bp.label = item.label;
    bp.icon = item.icon;
    bp.iconAsset = item.iconAsset;
    bp.action = item.action;
    bp.value = item.value;
    bp.inputMask = props.inputMask;
    bp.state = item.state;
    bp.text = props.text;
    bp.styles = props.styles;
    bp.minTouchSize = props.height;
    button(frame, row.rect(i), bp);
  }
  if (props.swipeLeft != NO_ACTION) {
    frame.hit(rect, props.swipeLeft, 0, InputSwipeLeft, StateNormal);
  }
  if (props.swipeRight != NO_ACTION) {
    frame.hit(rect, props.swipeRight, 0, InputSwipeRight, StateNormal);
  }
}

}  // namespace ui
}  // namespace freeink
