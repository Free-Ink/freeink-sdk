#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

// Standalone on/off switch widget (track + sliding knob). The list toggleRow
// composes a setting row around one of these; use this directly to place a
// switch anywhere (e.g. next to a slider label). `rect` gives the switch's
// top-left; its size comes from width/height (rect.width/height are ignored).
struct ToggleProps {
  bool checked = false;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  uint16_t inputMask = InputDefault;
  int16_t minTouchSize = 44;
  int16_t width = 38;
  int16_t height = 18;
  uint8_t radius = 0;
  uint8_t knobRadius = 0;
  int16_t knobInset = 3;
  uint8_t borderWidth = 1;
  Paint track = Paint::solid(Color::White);
  Paint checkedTrack = Paint::solid(Color::Black);
  Paint border = Paint::solid(Color::Black);
  Paint knob = Paint::solid(Color::Black);
  Paint checkedKnob = Paint::solid(Color::White);
  bool enabled = true;
};

template <size_t MaxInteractions>
void toggle(Frame<MaxInteractions> &frame, Rect rect,
            const ToggleProps &props) {
  const int16_t w = props.width < 18 ? 18 : props.width;
  const int16_t h = props.height < 12 ? 12 : props.height;
  Rect sw{rect.x, rect.y, w, h};

  if (props.enabled && props.action != NO_ACTION) {
    frame.hit(ensureMinTouchRect(sw, props.minTouchSize, frame.screen()),
              props.action, props.value, props.inputMask);
  }

  const uint8_t trackRadius =
      static_cast<uint8_t>(props.radius > h / 2 ? h / 2 : props.radius);
  frame.target().fill(sw, props.checked ? props.checkedTrack : props.track,
                      trackRadius);
  if (props.border.kind != PaintKind::None && props.borderWidth > 0) {
    frame.target().stroke(sw, props.border, props.borderWidth, trackRadius);
  }

  const int16_t inset = props.knobInset < 0 ? 0 : props.knobInset;
  const int16_t knobH = static_cast<int16_t>(h - inset * 2);
  const int16_t knobW = knobH;
  if (knobH <= 0)
    return;
  Rect knob{static_cast<int16_t>(props.checked ? sw.right() - inset - knobW
                                               : sw.x + inset),
            static_cast<int16_t>(sw.y + inset), knobW, knobH};
  const uint8_t knobRadius = static_cast<uint8_t>(
      props.knobRadius > knobH / 2 ? knobH / 2 : props.knobRadius);
  frame.target().fill(knob, props.checked ? props.checkedKnob : props.knob,
                      knobRadius);
}

} // namespace ui
} // namespace freeink
