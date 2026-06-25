#pragma once

#include "../../FreeInkUICore.h"
#include "../lists/setting-row.h"

namespace freeink {
namespace ui {

struct ToggleRowProps {
  SettingRowProps row{};
  bool checked = false;
  ActionId toggleAction = NO_ACTION;
  int16_t toggleValue = 0;
  int16_t toggleWidth = 38;
  int16_t toggleHeight = 18;
  uint8_t radius = 0;
  uint8_t knobRadius = 0;
  int16_t knobInset = 3;
  uint8_t borderWidth = 1;
  Paint track = Paint::solid(Color::White);
  Paint checkedTrack = Paint::solid(Color::Black);
  Paint border = Paint::solid(Color::Black);
  Paint knob = Paint::solid(Color::Black);
  Paint checkedKnob = Paint::solid(Color::White);
};

template <size_t MaxInteractions>
void toggleRow(Frame<MaxInteractions>& frame, Rect rect, const ToggleRowProps& props) {
  SettingRowProps row = props.row;
  row.value = nullptr;
  if (row.action == NO_ACTION) {
    row.action = props.toggleAction;
    row.valueId = props.toggleValue;
  }
  settingRow(frame, rect, row);

  const int16_t toggleW = props.toggleWidth < 18 ? 18 : props.toggleWidth;
  const int16_t toggleH = props.toggleHeight < 12 ? 12 : props.toggleHeight;
  Rect toggleRect{static_cast<int16_t>(rect.right() - row.sidePadding - toggleW),
                  static_cast<int16_t>(rect.y + (rect.height - toggleH) / 2), toggleW, toggleH};
  const uint8_t trackRadius = static_cast<uint8_t>(
      props.radius > toggleH / 2 ? toggleH / 2 : props.radius);
  frame.target().fill(toggleRect, props.checked ? props.checkedTrack : props.track, trackRadius);
  if (props.border.kind != PaintKind::None && props.borderWidth > 0) {
    frame.target().stroke(toggleRect, props.border, props.borderWidth, trackRadius);
  }

  const int16_t inset = props.knobInset < 0 ? 0 : props.knobInset;
  const int16_t knobH = static_cast<int16_t>(toggleH - inset * 2);
  const int16_t knobW = knobH;
  if (knobH <= 0) return;
  Rect knob{static_cast<int16_t>(props.checked ? toggleRect.right() - inset - knobW : toggleRect.x + inset),
            static_cast<int16_t>(toggleRect.y + inset), knobW, knobH};
  const uint8_t knobRadius = static_cast<uint8_t>(
      props.knobRadius > knobH / 2 ? knobH / 2 : props.knobRadius);
  frame.target().fill(knob, props.checked ? props.checkedKnob : props.knob, knobRadius);
}

}  // namespace ui
}  // namespace freeink
