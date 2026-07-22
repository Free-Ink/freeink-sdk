#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct TabItem {
  const char* label = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  int16_t value = 0;
  bool selected = false;
  bool enabled = true;
};

inline TabItem tabItem(const int value, const bool selected = false, const bool enabled = true,
                       const char* label = nullptr) {
  TabItem item;
  item.label = label;
  item.value = clampI16(value, -32768);
  item.selected = selected;
  item.enabled = enabled;
  return item;
}

using TabIconPainter = bool (*)(DrawTarget& target, Rect rect, const TabItem& tab, uint8_t index, void* userData);

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
  // Padding inside the tab pill before laying out icon/label.
  Insets contentInset{4, 4, 4, 4};
  int16_t gap = 0;
  int16_t iconSize = 0;
  int16_t minTouchSize = 44;
  TabIconPainter iconPainter = nullptr;
  void* iconPainterUserData = nullptr;
  int16_t selectedDotSize = 0;
  int16_t selectedDotInsetBottom = 4;
  Paint selectedDotPaint = Paint::solid(Color::Black);
  // Underline across the selected tab's pill bottom (the Lyra unfocused-tab
  // treatment); 0 = none.
  int16_t selectedUnderline = 0;
  Paint selectedUnderlinePaint = Paint::solid(Color::Black);
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
  const int16_t gap = props.gap > 0 ? props.gap : 0;
  const int16_t slotW = static_cast<int16_t>((rect.width - gap * (props.count - 1)) / props.count);
  for (uint8_t i = 0; i < props.count; ++i) {
    const TabItem& tab = props.tabs[i];
    const int16_t slotX = static_cast<int16_t>(rect.x + i * (slotW + gap));
    Rect slot{slotX, rect.y,
              static_cast<int16_t>(i == props.count - 1 ? rect.right() - slotX : slotW),
              static_cast<int16_t>(rect.height - dividerH)};
    Rect pill = slot.inset(props.tabInset);
    if (props.contentInset.left > 0 || props.contentInset.right > 0) {
      const int16_t labelW = tab.label && tab.label[0] != '\0'
                                 ? frame.target().measureText(props.text.font, tab.label, props.text).width
                                 : 0;
      BitmapRef measuredIcon = tab.icon ? tab.icon : resolveBitmap(frame.assets(), tab.iconAsset);
      const bool hasMeasuredIcon = measuredIcon || props.iconPainter != nullptr;
      const int16_t iconW = hasMeasuredIcon ? (props.iconSize > 0 ? props.iconSize
                                                                  : static_cast<int16_t>(measuredIcon ? measuredIcon.width : 16))
                                            : 0;
      const int16_t contentW = labelW > iconW ? labelW : iconW;
      const int16_t minW = pill.height;
      int16_t wantedW = static_cast<int16_t>(contentW + props.contentInset.left + props.contentInset.right);
      if (wantedW < minW) wantedW = minW;
      if (wantedW < pill.width) {
        pill.x = static_cast<int16_t>(pill.x + (pill.width - wantedW) / 2);
        pill.width = wantedW;
      }
    }
    if (pill.empty()) pill = slot;
    State state = tab.selected ? StateSelected : StateNormal;
    if (!tab.enabled) state |= StateDisabled;
    if (tab.enabled && props.action != NO_ACTION) {
      frame.hit(ensureMinTouchRect(slot, props.minTouchSize, frame.screen()), props.action, tab.value,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, tab.value, state);
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(pill, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(pill, style.border, style.borderWidth, style.radius, style.corners);
    }
    if (tab.selected && props.selectedUnderline > 0) {
      frame.target().fill(Rect{pill.x, static_cast<int16_t>(pill.bottom() - props.selectedUnderline), pill.width,
                               props.selectedUnderline},
                          props.selectedUnderlinePaint);
    }
    if (tab.selected && props.selectedDotSize > 0) {
      const int16_t dot = props.selectedDotSize;
      frame.target().fill(Rect{static_cast<int16_t>(slot.x + (slot.width - dot) / 2),
                               static_cast<int16_t>(slot.bottom() - props.selectedDotInsetBottom - dot),
                               dot, dot},
                          props.selectedDotPaint, static_cast<uint8_t>(dot / 2));
    }
    Rect content = pill.inset(props.contentInset);
    if (content.empty()) content = pill;
    BitmapRef icon = tab.icon ? tab.icon : resolveBitmap(frame.assets(), tab.iconAsset);
    const bool hasIcon = icon || props.iconPainter != nullptr;
    const bool hasLabel = tab.label != nullptr && tab.label[0] != '\0';
    if (hasIcon) {
      const int16_t iconSize =
          props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon ? (icon.height > 0 ? icon.height : icon.width) : 16);
      int16_t iconY = static_cast<int16_t>(content.y + (content.height - iconSize) / 2);
      if (hasLabel) {
        const int16_t labelH = frame.target().lineHeight(props.text.font);
        const int16_t stackH = static_cast<int16_t>(iconSize + 2 + labelH);
        iconY = static_cast<int16_t>(content.y + (content.height - stackH) / 2);
        if (iconY < content.y) iconY = content.y;
      }
      Rect iconRect{static_cast<int16_t>(content.x + (content.width - iconSize) / 2), iconY, iconSize, iconSize};
      bool drawn = false;
      if (props.iconPainter) drawn = props.iconPainter(frame.target(), iconRect, tab, i, props.iconPainterUserData);
      if (!drawn && icon) frame.target().bitmap(iconRect, icon, BitmapMode::Center, style.foreground);
    }
    if (hasLabel) {
      TextStyle label = textStyleWithForeground(props.text, style.foreground);
      label.align = TextAlign::Center;
      Rect labelRect = content;
      if (hasIcon) {
        const int16_t iconSize =
            props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon ? (icon.height > 0 ? icon.height : icon.width) : 16);
        const int16_t labelH = frame.target().lineHeight(label.font);
        const int16_t stackH = static_cast<int16_t>(iconSize + 2 + labelH);
        const int16_t stackY = static_cast<int16_t>(content.y + (content.height - stackH) / 2);
        labelRect = Rect{content.x, static_cast<int16_t>((stackY < content.y ? content.y : stackY) + iconSize + 2),
                         content.width, labelH};
      }
      frame.target().text(labelRect, tab.label, label);
    }
  }
  if (props.divider) {
    frame.target().fill(Rect{rect.x, static_cast<int16_t>(rect.bottom() - 1), rect.width, 1}, props.dividerPaint);
  }
}

}  // namespace ui
}  // namespace freeink
