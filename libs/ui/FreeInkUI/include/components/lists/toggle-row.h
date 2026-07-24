#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/toggle.h"
#include "../lists/setting-row.h"

namespace freeink {
namespace ui {

struct ToggleRowProps {
  SettingRowProps row{};
  bool checked = false;
  ActionId toggleAction = NO_ACTION;
  int16_t toggleValue = 0;
  // By default the whole row is the tap target. Set this when the row hosts
  // other interactions (or toggling must be deliberate): only the switch
  // itself — expanded to row.minTouchSize — responds to taps.
  bool hitToggleOnly = false;
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
void toggleRow(Frame<MaxInteractions> &frame, Rect rect,
               const ToggleRowProps &props) {
  SettingRowProps row = props.row;
  row.value = nullptr;
  if (!props.hitToggleOnly && row.action == NO_ACTION) {
    row.action = props.toggleAction;
    row.valueId = props.toggleValue;
  }
  settingRow(frame, rect, row);

  const int16_t toggleW = props.toggleWidth < 18 ? 18 : props.toggleWidth;
  const int16_t toggleH = props.toggleHeight < 12 ? 12 : props.toggleHeight;
  // The switch aligns to the label's title band (mirroring settingRow's slot
  // layout), leaving the full width under it for the subtitle.
  int16_t bandY = rect.y;
  int16_t bandH = rect.height;
  if (props.row.subtitle) {
    const int16_t labelH = frame.target().lineHeight(props.row.labelText.font);
    const int16_t subH = frame.target().lineHeight(props.row.subtitleText.font);
    const int16_t gap =
        props.row.titleSubtitleGap > 0 ? props.row.titleSubtitleGap : 0;
    int16_t top =
        static_cast<int16_t>(rect.y + (rect.height - labelH - gap - subH) / 2);
    if (top < rect.y)
      top = rect.y;
    bandY = top;
    bandH = labelH;
  }
  const Rect toggleRect{
      static_cast<int16_t>(rect.right() - row.sidePadding - toggleW),
      static_cast<int16_t>(bandY + (bandH - toggleH) / 2), toggleW, toggleH};
  // The switch itself is the shared standalone control; the row only decides
  // where it sits and whether the switch owns the hit (hitToggleOnly) or the
  // whole row does (handled by settingRow above).
  ToggleProps sw;
  sw.checked = props.checked;
  sw.action = props.hitToggleOnly ? props.toggleAction : NO_ACTION;
  sw.value = props.toggleValue;
  sw.minTouchSize = row.minTouchSize;
  sw.width = toggleW;
  sw.height = toggleH;
  sw.radius = props.radius;
  sw.knobRadius = props.knobRadius;
  sw.knobInset = props.knobInset;
  sw.borderWidth = props.borderWidth;
  sw.track = props.track;
  sw.checkedTrack = props.checkedTrack;
  sw.border = props.border;
  sw.knob = props.knob;
  sw.checkedKnob = props.checkedKnob;
  toggle(frame, toggleRect, sw);
}

} // namespace ui
} // namespace freeink
