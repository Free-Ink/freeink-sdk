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
  // Optional localized labels for Ok/Shift/Mode keys (see KeyboardProps::okLabel).
  const char* okLabel = nullptr;
  const char* shiftLabel = nullptr;
  const char* modeLabel = nullptr;
  uint16_t inputMask = InputDefault;
  int16_t selectedIndex = -1;
  // Use the body font for primary labels; alternates retain the small font.
  TextStyle labelText{FONT_SLOT_BODY};
  TextStyle altText{};
  StyleSet keyStyles{};
  Insets padding{5, 2, 5, 2};
  int16_t gap = 2;
  int16_t minTouchSize = 28;
  uint8_t keyRadius = 3;
  int16_t bottomHitOverflow = 0;
  KeyboardLayoutId layout = KeyboardLayoutId::QwertyEn;
  bool shifted = false;
  bool symbols = false;
  bool numberRow = false;
  bool inactiveSelection = false;
  bool langKey = false;
  ActionId langAction = NO_ACTION;
  TextStyle controlText{};
  int16_t rowGap = 6;
  int16_t altHintRightPadding = 10;
  int16_t altLabelGap = 4;
  int16_t digitLabelOffsetX = -4;
  // Appended fields preserve positional aggregate initialization.
  Paint background = Paint::none();
  const char* spaceLabel = nullptr;
  bool uniformKeyWidth = false;
};

// Mirror a KeyboardEntry's layer state into the props for this frame.
inline void applyEntry(QwertyKeyboardProps& props, const KeyboardEntry& entry) {
  props.layout = entry.layout;
  props.shifted = entry.shifted;
  props.symbols = entry.symbols;
  props.numberRow = entry.numberRow;
}

template <size_t MaxInteractions>
void qwertyKeyboard(Frame<MaxInteractions>& frame, Rect rect, const QwertyKeyboardProps& props) {
  KeyboardProps keyboardProps;
  keyboardProps.layout = &builtinKeyboardLayout(props.layout, props.shifted, props.symbols, props.numberRow, props.langKey);
  keyboardProps.keyAction = props.keyAction;
  keyboardProps.shiftAction = props.shiftAction;
  keyboardProps.modeAction = props.modeAction;
  keyboardProps.langAction = props.langAction;
  keyboardProps.deleteAction = props.deleteAction;
  keyboardProps.okAction = props.okAction;
  keyboardProps.okLabel = props.okLabel;
  keyboardProps.shiftLabel = props.shiftLabel;
  keyboardProps.modeLabel = props.modeLabel;
  keyboardProps.inputMask = props.inputMask;
  keyboardProps.selectedIndex = props.selectedIndex;
  keyboardProps.labelText = props.labelText;
  keyboardProps.controlText = props.controlText;
  keyboardProps.altText = props.altText;
  keyboardProps.background = props.background;
  keyboardProps.altHintRightPadding = props.altHintRightPadding;
  keyboardProps.altLabelGap = props.altLabelGap;
  keyboardProps.digitLabelOffsetX = props.digitLabelOffsetX;
  keyboardProps.spaceLabel = props.spaceLabel;
  keyboardProps.uniformKeyWidth = props.uniformKeyWidth;
  keyboardProps.keyStyles = props.keyStyles;
  keyboardProps.padding = props.padding;
  keyboardProps.gap = props.gap;
  keyboardProps.rowGap = props.rowGap;
  keyboardProps.minTouchSize = props.minTouchSize;
  keyboardProps.keyRadius = props.keyRadius;
  keyboardProps.bottomHitOverflow = props.bottomHitOverflow;
  keyboardProps.inactiveSelection = props.inactiveSelection;
  keyboard(frame, rect, keyboardProps);
}

}  // namespace ui
}  // namespace freeink
