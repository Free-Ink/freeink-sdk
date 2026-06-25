#pragma once

#include "../../FreeInkUICore.h"
#include "../keyboard/keyboard.h"

namespace freeink {
namespace ui {

struct QwertyKeyboardProps {
  ActionId keyAction = NO_ACTION;
  ActionId shiftAction = NO_ACTION;
  ActionId modeAction = NO_ACTION;
  ActionId deleteAction = NO_ACTION;
  ActionId okAction = NO_ACTION;
  uint16_t inputMask = InputDefault;
  int16_t selectedIndex = -1;
  TextStyle labelText{};
  StyleSet keyStyles{};
  Insets padding{5, 5, 5, 5};
  int16_t gap = 3;
  int16_t minTouchSize = 28;
  uint8_t keyRadius = 0;
  KeyboardLayoutId layout = KeyboardLayoutId::QwertyEn;
  bool shifted = false;
  bool symbols = false;
  bool inactiveSelection = false;
};

template <size_t MaxInteractions>
void qwertyKeyboard(Frame<MaxInteractions>& frame, Rect rect, const QwertyKeyboardProps& props) {
  KeyboardProps keyboardProps;
  keyboardProps.layout = &builtinKeyboardLayout(props.layout, props.shifted, props.symbols);
  keyboardProps.keyAction = props.keyAction;
  keyboardProps.shiftAction = props.shiftAction;
  keyboardProps.modeAction = props.modeAction;
  keyboardProps.deleteAction = props.deleteAction;
  keyboardProps.okAction = props.okAction;
  keyboardProps.inputMask = props.inputMask;
  keyboardProps.selectedIndex = props.selectedIndex;
  keyboardProps.labelText = props.labelText;
  keyboardProps.keyStyles = props.keyStyles;
  keyboardProps.padding = props.padding;
  keyboardProps.gap = props.gap;
  keyboardProps.minTouchSize = props.minTouchSize;
  keyboardProps.keyRadius = props.keyRadius;
  keyboardProps.inactiveSelection = props.inactiveSelection;
  keyboard(frame, rect, keyboardProps);
}

}  // namespace ui
}  // namespace freeink
