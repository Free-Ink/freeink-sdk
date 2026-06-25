#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct ProgressBarProps {
  int32_t value = 0;
  int32_t max = 100;
  Paint track = Paint::none();
  Paint fill = Paint::solid(Color::Black);
  Paint border = Paint::none();
  uint8_t borderWidth = 0;
  uint8_t radius = 0;
  // Minimum fill width for any nonzero value, so chart-style bars (reading
  // stats time-of-day/day-of-week) stay visible when a value rounds to 0px.
  int16_t minFill = 0;
};

template <size_t MaxInteractions>
void progressBar(Frame<MaxInteractions>& frame, Rect rect, const ProgressBarProps& props) {
  frame.target().fill(rect, props.track);
  if (props.border.kind != PaintKind::None && props.borderWidth > 0) {
    frame.target().stroke(rect, props.border, props.borderWidth, props.radius);
  }
  if (props.max <= 0 || props.value <= 0) return;
  const int32_t clamped = props.value > props.max ? props.max : props.value;
  int16_t fillWidth = static_cast<int16_t>((static_cast<int32_t>(rect.width) * clamped) / props.max);
  if (fillWidth < props.minFill) fillWidth = props.minFill > rect.width ? rect.width : props.minFill;
  if (fillWidth > 0) {
    frame.target().fill(Rect{rect.x, rect.y, fillWidth, rect.height}, props.fill);
  }
}

}  // namespace ui
}  // namespace freeink
