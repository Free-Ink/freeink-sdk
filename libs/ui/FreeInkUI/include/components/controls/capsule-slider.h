#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

// Finger-height filled-capsule slider (the iOS brightness control): one
// unbroken stadium outline, filled solid up to the value, with a round handle
// riding the boundary between the filled and empty track. Distinct from
// slider(), which pairs a thin progress track with a separate knob — on a
// capsule this tall that combination breaks the outline at the left end and
// hides a dark knob inside the dark fill. Here the fill edge IS the handle.
struct CapsuleSliderProps {
  int32_t value = 0;
  int32_t max = 100;
  ActionId action = NO_ACTION;
  int16_t actionValue = 0;
  // Touch + drag only by default: the capsule is a finger control. Boards that
  // need button access are better served by explicit step buttons beside it
  // (see sliderRow), which can repeat while held and land on exact values.
  uint16_t inputMask = InputTouch | InputDrag;
  Paint track = Paint::solid(Color::White);
  Paint fill = Paint::solid(Color::Black);
  Paint handle = Paint::solid(Color::White);
  Paint border = Paint::solid(Color::Black);
  int16_t stroke = 2;
  // Corner radius. A value >= half the rect height draws the classic full
  // stadium; smaller values square the track, fill, and handle toward a
  // theme's card language (0 = rectangular). RADIUS_INHERIT: Screen::
  // capsuleSlider()/sliderRow() substitute the theme's capsuleRadius; on a
  // bare Frame 0xFF already exceeds any half-height, so the stadium stands.
  // The handle geometry (half-height width, travel span) is shape-independent.
  uint8_t radius = RADIUS_INHERIT;
  bool enabled = true;
};

template <size_t MaxInteractions>
void capsuleSlider(Frame<MaxInteractions> &frame, Rect rect, const CapsuleSliderProps &props) {
  const int16_t stroke = props.stroke < 1 ? 1 : props.stroke;
  // Below the width of the handle itself there is no capsule to draw — the
  // handle would spill past the rect, and a track thinner than the outline
  // would put a negative width into fill(). Nothing registers either: at that
  // width there is no track a finger could meaningfully drag.
  const int16_t minTrack = static_cast<int16_t>(rect.height + 2 * stroke);
  if (rect.width < minTrack || rect.height <= 2 * stroke) return;

  if (props.enabled && props.action != NO_ACTION) {
    frame.hit(rect, props.action, props.actionValue, props.inputMask);
  }

  const int16_t halfOuter = static_cast<int16_t>(rect.height / 2);
  const uint8_t radius =
      props.radius >= halfOuter ? static_cast<uint8_t>(halfOuter) : props.radius;
  frame.target().fill(rect, props.track, radius);

  const int32_t max = props.max <= 0 ? 1 : props.max;
  int32_t value = props.value < 0 ? 0 : props.value;
  if (value > max) value = max;

  // The fill sits INSIDE the outline (inset by the stroke width) so the
  // capsule reads as one unbroken shape at every value instead of the fill
  // riding over its own border.
  const Rect inner = rect.inset(Insets{stroke, stroke, stroke, stroke});
  const int16_t cap = static_cast<int16_t>(inner.height / 2);
  const int16_t travel = static_cast<int16_t>(inner.width - 2 * cap);
  const int16_t handleCx =
      static_cast<int16_t>(inner.x + cap + (static_cast<int32_t>(travel) * value) / max);
  // The fill runs to the handle's far edge as a full stadium, so its right cap
  // is a semicircle about the handle's own center and radius — entirely under
  // the handle, touching the capsule only where the handle does. Ending the
  // fill at the handle's center instead (or squaring its right edge) leaves
  // its corners poking out past the round handle.
  int16_t fillW = static_cast<int16_t>(handleCx + cap - inner.x);
  if (fillW > inner.width) fillW = inner.width;
  // The fill and handle round by the same themed radius, clamped to the
  // half-height cap the stadium uses.
  const uint8_t innerRadius = props.radius >= cap ? static_cast<uint8_t>(cap) : props.radius;
  const Paint fill = props.enabled ? props.fill : Paint::dither(Color::LightGray);
  frame.target().fill(Rect{inner.x, inner.y, fillW, inner.height}, fill, innerRadius);
  frame.target().stroke(rect, props.border, static_cast<uint8_t>(stroke), radius);
  // The handle is drawn in the track color with its own outline so it stays
  // visible against the fill on one side and the empty track on the other.
  const Rect handle{static_cast<int16_t>(handleCx - cap), inner.y, static_cast<int16_t>(cap * 2),
                    inner.height};
  frame.target().fill(handle, props.handle, innerRadius);
  frame.target().stroke(handle, props.border, static_cast<uint8_t>(stroke), innerRadius);
}

}  // namespace ui
}  // namespace freeink
