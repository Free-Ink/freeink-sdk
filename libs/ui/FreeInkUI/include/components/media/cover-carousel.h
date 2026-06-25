#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

// One laid-out carousel slot. The component owns geometry, selection chrome,
// and input routing; the app draws the cover art into `content` (image
// decoding stays app-owned, and deterministic rects keep app-side frame
// caching valid).
struct CarouselSlot {
  bool valid = false;
  bool isCenter = false;
  int16_t itemIndex = -1;
  Rect frame{};    // outer frame (selection chrome drawn here)
  Rect content{};  // inside the frame inset; render the cover art here
};

struct CarouselProps {
  uint16_t count = 0;
  int16_t selectedIndex = 0;
  // Tapping any cover fires this action with the item index as value;
  // prev/next buttons and swipes re-fire it with the neighbor indexes.
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault;
  Size centerSize{220, 320};
  Size sideSize{120, 180};
  int16_t gap = 12;
  int16_t contentInset = 4;
  // Wrap to the other end at the edges; otherwise edge slots are invalid.
  bool wrap = false;
  // normal styles the side frames, selected styles the center frame.
  StyleSet frameStyles{};
};

// Lays out a prev/center/next cover carousel, draws the frames and selection
// chrome, and routes taps (cover → its index), prev/next buttons, and swipes
// (→ neighbor indexes). Returns the slots in `slots[3]` (0 = previous,
// 1 = center, 2 = next) so the app renders cover art into each
// `slots[i].content`.
template <size_t MaxInteractions>
void coverCarousel(Frame<MaxInteractions>& frame, Rect rect, const CarouselProps& props, CarouselSlot slots[3]) {
  for (int i = 0; i < 3; ++i) slots[i] = CarouselSlot{};
  if (props.count == 0) return;

  const auto neighbor = [&](const int16_t delta) -> int16_t {
    const int32_t raw = props.selectedIndex + delta;
    if (raw >= 0 && raw < props.count) return static_cast<int16_t>(raw);
    if (!props.wrap || props.count < 2) return -1;
    return static_cast<int16_t>((raw + props.count) % props.count);
  };
  slots[0].itemIndex = neighbor(-1);
  slots[1].itemIndex = props.selectedIndex;
  slots[1].isCenter = true;
  slots[2].itemIndex = neighbor(1);

  const int16_t centerX = static_cast<int16_t>(rect.x + (rect.width - props.centerSize.width) / 2);
  const int16_t midY = static_cast<int16_t>(rect.y + rect.height / 2);
  slots[1].frame = Rect{centerX, static_cast<int16_t>(midY - props.centerSize.height / 2), props.centerSize.width,
                        props.centerSize.height};
  slots[0].frame = Rect{static_cast<int16_t>(centerX - props.gap - props.sideSize.width),
                        static_cast<int16_t>(midY - props.sideSize.height / 2), props.sideSize.width,
                        props.sideSize.height};
  slots[2].frame = Rect{static_cast<int16_t>(slots[1].frame.right() + props.gap),
                        static_cast<int16_t>(midY - props.sideSize.height / 2), props.sideSize.width,
                        props.sideSize.height};

  StyleSet styles = props.frameStyles;
  if (styles.unset()) {
    styles.normal.background = Paint::solid(Color::White);
    styles.selected = styles.normal;
    styles.selected.background = Paint::solid(Color::Black);
    styles.selected.foreground = Paint::solid(Color::White);
  }

  for (int i = 0; i < 3; ++i) {
    CarouselSlot& slot = slots[i];
    if (slot.itemIndex < 0) continue;
    slot.valid = true;
    slot.content = slot.frame.inset(
        Insets{props.contentInset, props.contentInset, props.contentInset, props.contentInset});
    if (props.action != NO_ACTION) {
      frame.hit(ensureMinTouchRect(slot.frame, frame.device().minTouchSize, frame.screen()), props.action,
                slot.itemIndex, props.inputMask, slot.isCenter ? StateSelected : StateNormal);
    }
    const BoxStyle& style = styles.resolve(slot.isCenter ? StateSelected : StateNormal);
    frame.target().fill(slot.frame, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(slot.frame, style.border, style.borderWidth, style.radius, style.corners);
    }
  }

  // Swipes and prev/next buttons step to the neighbors.
  if (props.action != NO_ACTION) {
    if (slots[0].itemIndex >= 0) {
      frame.hit(rect, props.action, slots[0].itemIndex, InputSwipeRight | InputPrev, StateNormal);
    }
    if (slots[2].itemIndex >= 0) {
      frame.hit(rect, props.action, slots[2].itemIndex, InputSwipeLeft | InputNext, StateNormal);
    }
  }
}

}  // namespace ui
}  // namespace freeink
