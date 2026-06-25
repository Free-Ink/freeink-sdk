#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct TapZone {
  Rect rect{};
  ActionId action = NO_ACTION;
  int16_t value = 0;
  uint16_t inputMask = InputTouch;
  State state = StateNormal;
  bool enabled = true;
};

struct TapZonesProps {
  const TapZone* zones = nullptr;
  uint8_t count = 0;
  ActionId swipeLeft = NO_ACTION;
  ActionId swipeRight = NO_ACTION;
  ActionId back = NO_ACTION;
};

template <size_t MaxInteractions>
void tapZones(Frame<MaxInteractions>& frame, Rect rect, const TapZonesProps& props) {
  if (props.zones) {
    for (uint8_t i = 0; i < props.count; ++i) {
      const TapZone& zone = props.zones[i];
      if (!zone.enabled || zone.action == NO_ACTION) continue;
      Rect zoneRect = zone.rect.empty() ? rect : zone.rect;
      frame.hit(zoneRect, zone.action, zone.value, zone.inputMask, zone.state);
    }
  }
  if (props.swipeLeft != NO_ACTION) frame.hit(rect, props.swipeLeft, 0, InputSwipeLeft, StateNormal);
  if (props.swipeRight != NO_ACTION) frame.hit(rect, props.swipeRight, 0, InputSwipeRight, StateNormal);
  if (props.back != NO_ACTION) frame.hit(rect, props.back, 0, InputBack, StateNormal);
}

}  // namespace ui
}  // namespace freeink
