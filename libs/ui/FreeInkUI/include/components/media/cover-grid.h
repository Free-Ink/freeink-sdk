#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct CoverGridItem {
  const char* title = nullptr;
  BitmapRef cover{};
  AssetRef coverAsset{};
  State state = StateNormal;
  int16_t actionValue = 0;
  bool enabled = true;
};

struct CoverGridProps {
  const CoverGridItem* items = nullptr;
  uint16_t count = 0;
  uint16_t topIndex = 0;
  int16_t selectedIndex = -1;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle titleText{};
  StyleSet cellStyles{};
  uint8_t columns = 3;
  Size coverSize{72, 104};
  int16_t rowHeight = 132;
  int16_t gap = 8;
  int16_t labelHeight = 20;
  int16_t minTouchSize = 44;
};

template <size_t MaxInteractions>
void coverGrid(Frame<MaxInteractions>& frame, Rect rect, const CoverGridProps& props) {
  if (!props.items || props.count == 0 || props.columns == 0) return;
  const int16_t cellW = static_cast<int16_t>((rect.width - (props.columns - 1) * props.gap) / props.columns);
  const int16_t strideY = static_cast<int16_t>(props.rowHeight + props.gap);
  const uint16_t rowsVisible = props.rowHeight > 0 ? static_cast<uint16_t>((rect.height + props.gap) / strideY) : 0;
  const uint16_t cellsVisible = static_cast<uint16_t>(rowsVisible * props.columns);
  uint16_t top = props.topIndex;
  if (top >= props.count) top = 0;
  for (uint16_t visible = 0; visible < cellsVisible && top + visible < props.count; ++visible) {
    const uint16_t index = static_cast<uint16_t>(top + visible);
    const uint8_t col = static_cast<uint8_t>(visible % props.columns);
    const uint16_t row = static_cast<uint16_t>(visible / props.columns);
    Rect cell{static_cast<int16_t>(rect.x + col * (cellW + props.gap)),
              static_cast<int16_t>(rect.y + row * strideY), cellW, props.rowHeight};
    const CoverGridItem& item = props.items[index];
    State state = item.state;
    if (props.selectedIndex == static_cast<int16_t>(index)) state |= StateSelected;
    if (!item.enabled) state |= StateDisabled;
    if (props.action != NO_ACTION && item.enabled) {
      frame.hit(ensureMinTouchRect(cell, props.minTouchSize, frame.screen()), props.action, item.actionValue,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, item.actionValue, state);
    StyleSet styles = props.cellStyles.unset() ? defaultListRowStyles() : props.cellStyles;
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(cell, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(cell, style.border, style.borderWidth, style.radius, style.corners);
    }
    Rect coverRect{static_cast<int16_t>(cell.x + (cell.width - props.coverSize.width) / 2), cell.y,
                   props.coverSize.width, props.coverSize.height};
    frame.target().fill(coverRect, Paint::dither(Color::LightGray));
    const BitmapRef cover = item.cover ? item.cover : resolveBitmap(frame.assets(), item.coverAsset);
    if (cover) frame.target().bitmap(coverRect, cover, BitmapMode::Cover, style.foreground);
    if (item.title && props.labelHeight > 0) {
      TextStyle title = textStyleWithForeground(props.titleText, style.foreground);
      title.align = TextAlign::Center;
      title.maxLines = title.maxLines > 0 ? title.maxLines : 1;
      frame.target().text(Rect{cell.x, static_cast<int16_t>(coverRect.bottom() + 2), cell.width, props.labelHeight},
                          item.title, title);
    }
  }
}

}  // namespace ui
}  // namespace freeink
