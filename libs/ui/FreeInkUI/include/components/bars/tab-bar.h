#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct TabItem {
  const char* label = nullptr;
  int16_t value = 0;
  bool selected = false;
};

struct TabBarProps {
  const TabItem* tabs = nullptr;
  uint8_t count = 0;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle text{};
  // selected state drives the pill: set selected.background + radius.
  StyleSet tabStyles{};
  // Inset of each tab pill within its equal-width slot.
  Insets tabInset{4, 4, 8, 4};
  // 1px divider along the bottom edge (RoundedRaff-style settings tabs).
  bool divider = false;
  Paint dividerPaint = Paint::solid(Color::Black);
};

template <size_t MaxInteractions>
void tabBar(Frame<MaxInteractions>& frame, Rect rect, const TabBarProps& props) {
  if (!props.tabs || props.count == 0) return;
  StyleSet styles = props.tabStyles;
  if (styles.unset()) {
    styles.selected.background = Paint::solid(Color::Black);
    styles.selected.foreground = Paint::solid(Color::White);
  }

  const int16_t dividerH = props.divider ? 1 : 0;
  const int16_t slotW = static_cast<int16_t>(rect.width / props.count);
  for (uint8_t i = 0; i < props.count; ++i) {
    const TabItem& tab = props.tabs[i];
    Rect slot{static_cast<int16_t>(rect.x + i * slotW), rect.y, slotW,
              static_cast<int16_t>(rect.height - dividerH)};
    Rect pill = slot.inset(props.tabInset);
    State state = tab.selected ? StateSelected : StateNormal;
    if (props.action != NO_ACTION) {
      frame.hit(ensureMinTouchRect(slot, frame.device().minTouchSize, frame.screen()), props.action, tab.value,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, tab.value, state);
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(pill, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(pill, style.border, style.borderWidth, style.radius, style.corners);
    }
    TextStyle label = textStyleWithForeground(props.text, style.foreground);
    label.align = TextAlign::Center;
    frame.target().text(pill, tab.label, label);
  }
  if (props.divider) {
    frame.target().fill(Rect{rect.x, static_cast<int16_t>(rect.bottom() - 1), rect.width, 1}, props.dividerPaint);
  }
}

}  // namespace ui
}  // namespace freeink
