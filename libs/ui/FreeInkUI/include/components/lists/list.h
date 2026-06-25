#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct ListItem {
  const char* label = nullptr;
  const char* subtitle = nullptr;
  const char* value = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  State state = StateNormal;
  int16_t actionValue = 0;
  bool enabled = true;
  // Section header row: shorter, non-interactive, drawn with headerText and
  // an underline; never selected or focused.
  bool isHeader = false;
};

enum class SelectionMarker : uint8_t {
  None,       // selection shown by the row's selected BoxStyle
  Underline,  // thin line under the selected row's content
  Triangle,   // right-pointing triangle at the selected row's left edge
};

struct ListProps {
  const ListItem* items = nullptr;
  uint16_t count = 0;
  // First item index drawn at the top of the rect. The list is virtualized:
  // only the rows that fully fit inside the rect are laid out, drawn, and
  // registered for interaction. Use listVisibleRows()/listTopIndexFor() to
  // keep a selection in view while scrolling.
  uint16_t topIndex = 0;
  int16_t selectedIndex = -1;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle labelText{};
  TextStyle subtitleText{};
  TextStyle valueText{};
  StyleSet rowStyles{};
  int16_t rowHeight = 36;
  int16_t rowGap = 0;
  uint8_t rowRadius = 0;
  int16_t sidePadding = 8;
  int16_t textGap = 6;
  int16_t iconSize = 0;
  int16_t scrollIndicatorWidth = 3;
  bool centerSingleLine = false;
  // Shrink each row's background/hit area to its label width plus side
  // padding instead of the full rect width (hug-content menu rows).
  bool hugContents = false;
  // Draws a thin position indicator along the right edge when the list
  // overflows the rect.
  bool scrollIndicator = true;
  // Additional marker drawn on the selected row (the v1 theme Underline and
  // Triangle selection styles).
  SelectionMarker selectionMarker = SelectionMarker::None;
  Paint markerPaint = Paint::solid(Color::Black);
  int16_t markerInset = 0;       // x offset of the triangle / underline start
  int16_t markerThickness = 2;   // underline thickness
  // Section header rows (ListItem::isHeader).
  TextStyle headerText{};
  int16_t headerRowHeight = 0;  // 0 = headerText line height + underline gap
  int16_t sectionGap = 16;      // extra padding above a non-first header
  bool headerUnderline = true;
};

template <size_t MaxInteractions>
void list(Frame<MaxInteractions>& frame, Rect rect, const ListProps& props) {
  if (!props.items || props.count == 0) return;
  const int16_t rowH = props.rowHeight > 0 ? props.rowHeight : 36;
  const uint16_t visible = listVisibleRows(rect, rowH, props.rowGap);
  const bool overflows = props.count > visible;
  uint16_t top = props.topIndex;
  if (top > props.count - 1) top = props.count - 1;
  if (overflows && top > props.count - visible) top = static_cast<uint16_t>(props.count - visible);
  if (!overflows) top = 0;
  const uint16_t end = overflows ? static_cast<uint16_t>(top + visible) : props.count;

  Rect rowArea = rect;
  if (props.scrollIndicator && overflows && props.scrollIndicatorWidth > 0) {
    rowArea.width = static_cast<int16_t>(rowArea.width - props.scrollIndicatorWidth - 2);
    Rect track{static_cast<int16_t>(rect.right() - props.scrollIndicatorWidth), rect.y, props.scrollIndicatorWidth,
               rect.height};
    frame.target().fill(track, Paint::dither(Color::LightGray));
    int16_t thumbH = static_cast<int16_t>((static_cast<int32_t>(rect.height) * visible) / props.count);
    if (thumbH < 12) thumbH = 12;
    const int32_t scrollRange = props.count - visible;
    const int16_t thumbY = static_cast<int16_t>(
        track.y + (scrollRange > 0 ? (static_cast<int32_t>(track.height - thumbH) * top) / scrollRange : 0));
    frame.target().fill(Rect{track.x, thumbY, track.width, thumbH}, Paint::solid(Color::Black));
  }

  // Cursor-based layout: section header rows are shorter than item rows, so
  // positions accumulate instead of multiplying a fixed stride.
  const int16_t headerLh = frame.target().lineHeight(props.headerText.font);
  const int16_t headerH = props.headerRowHeight > 0 ? props.headerRowHeight : static_cast<int16_t>(headerLh + 4);
  int16_t cursorY = rowArea.y;
  uint16_t drawnRows = 0;
  for (uint16_t i = top; i < props.count; ++i) {
    const ListItem& item = props.items[i];
    if (item.isHeader) {
      const int16_t pad = i != top ? props.sectionGap : 0;
      if (static_cast<int16_t>(cursorY + pad + headerH) > rowArea.bottom()) break;
      cursorY = static_cast<int16_t>(cursorY + pad);
      Rect headerRow{static_cast<int16_t>(rowArea.x + props.sidePadding), cursorY,
                     static_cast<int16_t>(rowArea.width - props.sidePadding * 2), headerLh};
      frame.target().text(headerRow, item.label, props.headerText);
      if (props.headerUnderline) {
        frame.target().fill(Rect{headerRow.x, static_cast<int16_t>(cursorY + headerLh + 2), headerRow.width, 1},
                            Paint::solid(props.headerText.color));
      }
      cursorY = static_cast<int16_t>(cursorY + headerH + props.rowGap);
      continue;
    }
    if (static_cast<int16_t>(cursorY + rowH) > rowArea.bottom() || drawnRows >= visible || i >= end) break;
    ++drawnRows;
    Rect row{rowArea.x, cursorY, rowArea.width, rowH};
    cursorY = static_cast<int16_t>(cursorY + rowH + props.rowGap);
    if (props.hugContents && item.label) {
      // Hug-content rows shrink to the label width plus padding so the
      // selection pill wraps the text instead of spanning the rect.
      const int16_t labelW = frame.target().measureText(props.labelText.font, item.label, props.labelText).width;
      const int16_t hugW = static_cast<int16_t>(labelW + props.sidePadding * 2);
      if (hugW < row.width) row.width = hugW;
    }
    State state = item.state;
    if (props.selectedIndex == static_cast<int16_t>(i)) state |= StateSelected;
    if (!item.enabled) state |= StateDisabled;
    if (props.action != NO_ACTION && item.enabled) {
      frame.hit(ensureMinTouchRect(row, frame.device().minTouchSize, frame.screen()), props.action, item.actionValue,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, item.actionValue, state);
    StyleSet styles = props.rowStyles.unset() ? defaultListRowStyles() : props.rowStyles;
    if (props.rowRadius > 0) setStyleRadius(styles, props.rowRadius);
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(row, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(row, style.border, style.borderWidth, style.radius, style.corners);
    }

    Rect content = row.inset(Insets{0, props.sidePadding, 0, props.sidePadding});
    const BitmapRef icon = item.icon ? item.icon : resolveBitmap(frame.assets(), item.iconAsset);
    if (icon) {
      const int16_t iconSize = props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon.width);
      Rect iconRect{content.x, static_cast<int16_t>(content.y + (content.height - iconSize) / 2), iconSize, iconSize};
      frame.target().bitmap(iconRect, icon, BitmapMode::Contain, style.foreground);
      content.x = static_cast<int16_t>(content.x + iconSize + props.textGap);
      content.width = static_cast<int16_t>(content.width - iconSize - props.textGap);
    }
    if (item.value) {
      TextStyle valueStyle = textStyleWithForeground(props.valueText, style.foreground);
      valueStyle.align = TextAlign::Right;
      const int16_t valueW = frame.target().measureText(valueStyle.font, item.value, valueStyle).width;
      Rect valueRect{static_cast<int16_t>(content.right() - valueW), content.y, valueW, content.height};
      frame.target().text(valueRect, item.value, valueStyle);
      content.width = static_cast<int16_t>(content.width - valueW - props.textGap);
    }
    if (item.subtitle) {
      const int16_t lh = frame.target().lineHeight(props.labelText.font);
      Rect labelRect{content.x, content.y, content.width, lh};
      Rect subRect{content.x, static_cast<int16_t>(content.y + lh), content.width,
                   static_cast<int16_t>(content.height - lh)};
      frame.target().text(labelRect, item.label, textStyleWithForeground(props.labelText, style.foreground));
      frame.target().text(subRect, item.subtitle, textStyleWithForeground(props.subtitleText, style.foreground));
    } else {
      TextStyle labelStyle = textStyleWithForeground(props.labelText, style.foreground);
      if (props.centerSingleLine) labelStyle.align = TextAlign::Center;
      frame.target().text(content, item.label, labelStyle);
    }

    if (props.selectedIndex == static_cast<int16_t>(i) && props.selectionMarker != SelectionMarker::None) {
      if (props.selectionMarker == SelectionMarker::Underline) {
        frame.target().fill(Rect{static_cast<int16_t>(row.x + props.sidePadding + props.markerInset),
                                 static_cast<int16_t>(row.bottom() - props.markerThickness),
                                 static_cast<int16_t>(row.width - props.sidePadding * 2 - props.markerInset),
                                 props.markerThickness},
                            props.markerPaint);
      } else {
        // 12x18 right-pointing triangle, vertically centered — the v1 theme
        // Triangle selection marker geometry.
        const int16_t tx = static_cast<int16_t>(row.x + props.markerInset);
        const int16_t cy = static_cast<int16_t>(row.y + row.height / 2);
        frame.target().triangle(Point{tx, static_cast<int16_t>(cy - 9)}, Point{tx, static_cast<int16_t>(cy + 9)},
                                Point{static_cast<int16_t>(tx + 12), cy}, props.markerPaint);
      }
    }
  }
}

}  // namespace ui
}  // namespace freeink
