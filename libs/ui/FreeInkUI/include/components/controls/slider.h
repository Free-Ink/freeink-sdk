#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/progress-bar.h"

namespace freeink {
namespace ui {

struct SliderProps {
  int32_t value = 0;
  int32_t max = 100;
  ActionId action = NO_ACTION;
  int16_t actionValue = 0;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  Paint track = Paint::dither(Color::LightGray);
  Paint fill = Paint::solid(Color::Black);
  Paint knob = Paint::solid(Color::Black);
  Paint border = Paint::solid(Color::Black);
  int16_t trackHeight = 4;
  int16_t knobWidth = 14;
  int16_t knobHeight = 22;
  int16_t horizontalPadding = 8;
  int16_t minTouchSize = 44;
  uint8_t radius = 0;
  bool enabled = true;
};

template <size_t MaxInteractions>
void slider(Frame<MaxInteractions>& frame, Rect rect, const SliderProps& props) {
  State state = props.enabled ? StateNormal : StateDisabled;
  if (props.enabled && props.action != NO_ACTION) {
    frame.hit(ensureMinTouchRect(rect, props.minTouchSize, frame.screen()), props.action, props.actionValue,
              props.inputMask, state);
  }
  state = frame.stateFor(props.action, props.actionValue, state);

  Rect content = rect.inset(Insets{0, props.horizontalPadding, 0, props.horizontalPadding});
  if (content.empty()) return;
  const int16_t trackH = props.trackHeight < 1 ? 1 : props.trackHeight;
  Rect track{content.x, static_cast<int16_t>(content.y + (content.height - trackH) / 2), content.width, trackH};
  ProgressBarProps bar;
  bar.value = props.value;
  bar.max = props.max;
  bar.track = props.track;
  bar.fill = hasState(state, StateDisabled) ? Paint::dither(Color::LightGray) : props.fill;
  bar.radius = props.radius;
  progressBar(frame, track, bar);

  const int32_t max = props.max <= 0 ? 1 : props.max;
  int32_t value = props.value < 0 ? 0 : props.value;
  if (value > max) value = max;
  const int16_t knobW = props.knobWidth < 4 ? 4 : props.knobWidth;
  const int16_t knobH = props.knobHeight < trackH ? trackH : props.knobHeight;
  const int16_t travel = content.width > knobW ? static_cast<int16_t>(content.width - knobW) : 0;
  const int16_t knobX = static_cast<int16_t>(content.x + (static_cast<int32_t>(travel) * value) / max);
  Rect knobRect{knobX, static_cast<int16_t>(content.y + (content.height - knobH) / 2), knobW, knobH};
  frame.target().fill(knobRect, hasState(state, StateDisabled) ? Paint::dither(Color::LightGray) : props.knob,
                      props.radius);
  if (props.border.kind != PaintKind::None) frame.target().stroke(knobRect, props.border, 1, props.radius);
}

}  // namespace ui
}  // namespace freeink
