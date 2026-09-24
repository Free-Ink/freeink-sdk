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

inline CoverGridItem coverGridItem(const char* title, const int actionValue, const bool enabled = true,
                                   State state = StateNormal) {
  CoverGridItem item;
  item.title = title;
  item.actionValue = clampI16(actionValue, -32768);
  item.enabled = enabled;
  item.state = state;
  return item;
}

using CoverGridCoverPainter = bool (*)(DrawTarget& target, Rect rect, const CoverGridItem& item, uint16_t index,
                                       void* userData);
using CoverGridItemProvider = CoverGridItem (*)(uint16_t index, void* userData);

enum class CoverGridSelectionIndicator {
  Cell,
  CoverFrame,
};

enum class CoverGridColumnLayout : uint8_t { EqualWidth, SpaceBetween };

struct CoverGridProps {
  const CoverGridItem* items = nullptr;
  CoverGridItemProvider itemProvider = nullptr;
  void* itemProviderUserData = nullptr;
  uint16_t count = 0;
  uint16_t topIndex = 0;
  int16_t selectedIndex = -1;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle titleText{};
  StyleSet cellStyles{};
  CoverGridSelectionIndicator selectionIndicator = CoverGridSelectionIndicator::Cell;
  uint8_t columns = 3;
  CoverGridColumnLayout columnLayout = CoverGridColumnLayout::EqualWidth;
  Size coverSize{72, 104};
  int16_t rowHeight = 132;
  int16_t gap = 8;
  int16_t rowGap = -1;
  Insets cellInset{};
  Insets labelInset{};
  int16_t labelHeight = 20;
  int16_t labelGap = 2;
  TextAlign labelAlign = TextAlign::Center;
  // Constrain the label to the cover slot rather than the full cell content.
  bool labelFollowsCover = false;
  int16_t minTouchSize = 44;
  bool scrollIndicator = true;
  int16_t scrollIndicatorWidth = 3;
  int16_t scrollIndicatorGap = 4;
  int16_t selectedCoverFrameGap = 3;
  int16_t selectedCoverFrameWidth = 2;
  uint8_t selectedCoverFrameRadius = 0;
  CoverGridCoverPainter coverPainter = nullptr;
  void* coverPainterUserData = nullptr;
};

inline uint16_t coverGridVisibleCells(Rect rect, const uint8_t columns, const int16_t rowHeight, const int16_t rowGap) {
  if (columns == 0 || rowHeight <= 0) return 0;
  const int16_t gap = rowGap > 0 ? rowGap : 0;
  const int16_t strideY = static_cast<int16_t>(rowHeight + gap);
  const uint16_t rowsVisible = static_cast<uint16_t>((rect.height + gap) / strideY);
  return static_cast<uint16_t>(rowsVisible * columns);
}

inline uint16_t coverGridTopIndexFor(const uint16_t selectedIndex, const uint16_t count, const uint8_t columns,
                                     const uint16_t pageItems) {
  if (count == 0 || columns == 0 || pageItems == 0) return 0;
  const uint16_t selected = selectedIndex < count ? selectedIndex : static_cast<uint16_t>(count - 1);
  const uint16_t rowStart = static_cast<uint16_t>((selected / columns) * columns);
  return static_cast<uint16_t>((rowStart / pageItems) * pageItems);
}

template <size_t MaxInteractions>
void coverGrid(Frame<MaxInteractions>& frame, Rect rect, const CoverGridProps& props) {
  if ((!props.items && !props.itemProvider) || props.count == 0 || props.columns == 0) return;
  const int16_t rowGap = props.rowGap >= 0 ? props.rowGap : props.gap;
  const int16_t strideY = static_cast<int16_t>(props.rowHeight + rowGap);
  const uint16_t rowsVisible = props.rowHeight > 0 ? static_cast<uint16_t>((rect.height + rowGap) / strideY) : 0;
  const uint16_t cellsVisible = static_cast<uint16_t>(rowsVisible * props.columns);
  const bool overflows = cellsVisible > 0 && props.count > cellsVisible;
  Rect gridRect = rect;
  if (props.scrollIndicator && overflows && props.scrollIndicatorWidth > 0) {
    gridRect.width =
        static_cast<int16_t>(gridRect.width - props.scrollIndicatorWidth - props.scrollIndicatorGap);
    Rect track{static_cast<int16_t>(rect.right() - props.scrollIndicatorWidth), rect.y,
               props.scrollIndicatorWidth, rect.height};
    frame.target().line(Point{track.x, track.y}, Point{track.x, static_cast<int16_t>(track.bottom() - 1)}, 1,
                        Paint::solid(Color::Black));
    int32_t thumbHeight = 0;
    if (track.height > 0) {
      thumbHeight = (static_cast<int32_t>(track.height) * cellsVisible) / props.count;
      if (thumbHeight < props.scrollIndicatorWidth) thumbHeight = props.scrollIndicatorWidth;
    }
    const int16_t thumbH = static_cast<int16_t>(thumbHeight);
    const int32_t scrollRange = props.count - cellsVisible;
    const int16_t thumbY = static_cast<int16_t>(
        track.y + (scrollRange > 0 ? (static_cast<int32_t>(track.height - thumbH) * props.topIndex) / scrollRange
                                   : 0));
    frame.target().fill(Rect{static_cast<int16_t>(track.x - props.scrollIndicatorWidth), thumbY,
                             props.scrollIndicatorWidth, thumbH},
                        Paint::solid(Color::Black));
  }
  const int16_t naturalCellW = props.coverSize.width + props.cellInset.left + props.cellInset.right;
  const bool distribute = props.columnLayout == CoverGridColumnLayout::SpaceBetween && naturalCellW > 0 &&
                          static_cast<int32_t>(naturalCellW) * props.columns +
                              static_cast<int32_t>(props.columns - 1) * props.gap <= gridRect.width;
  const int16_t cellW = distribute ? naturalCellW
      : static_cast<int16_t>((gridRect.width - (props.columns - 1) * props.gap) / props.columns);
  uint16_t top = props.topIndex;
  if (top >= props.count) top = 0;
  for (uint16_t visible = 0; visible < cellsVisible && top + visible < props.count; ++visible) {
    const uint16_t index = static_cast<uint16_t>(top + visible);
    const uint8_t col = static_cast<uint8_t>(visible % props.columns);
    const uint16_t row = static_cast<uint16_t>(visible / props.columns);
    const int16_t columnOffset = distribute
        ? static_cast<int16_t>(props.columns > 1 ? static_cast<int32_t>(col) * (gridRect.width - cellW) / (props.columns - 1)
                                                : (gridRect.width - cellW) / 2)
        : static_cast<int16_t>(col * (cellW + props.gap));
    Rect cell{static_cast<int16_t>(gridRect.x + columnOffset),
              static_cast<int16_t>(gridRect.y + row * strideY), cellW, props.rowHeight};
    const CoverGridItem providedItem =
        props.itemProvider ? props.itemProvider(index, props.itemProviderUserData) : CoverGridItem{};
    const CoverGridItem& item = props.itemProvider ? providedItem : props.items[index];
    State state = item.state;
    if (props.selectedIndex == static_cast<int16_t>(index)) state |= StateSelected;
    if (!item.enabled) state |= StateDisabled;
    if (props.action != NO_ACTION && item.enabled) {
      frame.hit(ensureMinTouchRect(cell, props.minTouchSize, frame.screen()), props.action, item.actionValue,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, item.actionValue, state);
    const bool selected = (state & StateSelected) != 0;
    State cellState = state;
    if (selected && props.selectionIndicator == CoverGridSelectionIndicator::CoverFrame) {
      cellState = static_cast<State>(static_cast<uint8_t>(cellState) & ~static_cast<uint8_t>(StateSelected));
    }
    StyleSet styles = props.cellStyles.unset() ? defaultListRowStyles() : props.cellStyles;
    const BoxStyle& style = styles.resolve(cellState);
    const BoxStyle& selectedStyle = styles.resolve(state);
    frame.target().fill(cell, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(cell, style.border, style.borderWidth, style.radius, style.corners);
    }
    Rect content = cell.inset(props.cellInset);
    if (content.empty()) content = cell;
    Rect coverRect{static_cast<int16_t>(content.x + (content.width - props.coverSize.width) / 2), content.y,
                   props.coverSize.width, props.coverSize.height};
    bool coverDrawn = false;
    if (props.coverPainter) {
      coverDrawn = props.coverPainter(frame.target(), coverRect, item, index, props.coverPainterUserData);
    }
    if (!coverDrawn) {
      frame.target().fill(coverRect, Paint::dither(Color::LightGray));
      const BitmapRef cover = item.cover ? item.cover : resolveBitmap(frame.assets(), item.coverAsset);
      if (cover) frame.target().bitmap(coverRect, cover, BitmapMode::Cover, style.foreground);
    }
    if (selected && props.selectionIndicator == CoverGridSelectionIndicator::CoverFrame) {
      const int16_t gap = props.selectedCoverFrameGap;
      Rect frameRect{static_cast<int16_t>(coverRect.x - gap), static_cast<int16_t>(coverRect.y - gap),
                     static_cast<int16_t>(coverRect.width + gap * 2), static_cast<int16_t>(coverRect.height + gap * 2)};
      frame.target().stroke(frameRect, selectedStyle.border.kind != PaintKind::None ? selectedStyle.border
                                                                                    : Paint::solid(Color::Black),
                            props.selectedCoverFrameWidth, props.selectedCoverFrameRadius);
    }
    if (item.title && props.labelHeight > 0) {
      TextStyle title = textStyleWithForeground(props.titleText, style.foreground);
      title.align = props.labelAlign;
      title.maxLines = title.maxLines > 0 ? title.maxLines : 1;
      Rect labelRect{props.labelFollowsCover ? coverRect.x : content.x,
                     static_cast<int16_t>(coverRect.bottom() + props.labelGap),
                     props.labelFollowsCover ? coverRect.width : content.width, props.labelHeight};
      labelRect = labelRect.inset(props.labelInset);
      if (!labelRect.empty()) {
        const int16_t reservedHeight = static_cast<int16_t>(frame.target().lineHeight(title.font) * title.maxLines);
        if (reservedHeight > 0 && reservedHeight < labelRect.height) labelRect.height = reservedHeight;
        frame.target().text(labelRect, item.title, title);
      }
    }
  }
}

}  // namespace ui
}  // namespace freeink
