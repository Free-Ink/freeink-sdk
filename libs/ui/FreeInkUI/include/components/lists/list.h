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
  // On/off row: a switch (toggle-row visuals) replaces the value slot; the
  // value string is ignored when set. Activation stays row-level via action.
  bool toggle = false;
  bool toggleChecked = false;
};

enum class SelectionMarker : uint8_t {
  None,       // selection shown by the row's selected BoxStyle
  Underline,  // thin line under the selected row's content
  Triangle,   // right-pointing triangle at the selected row's left edge
  Bitmap,     // caller-supplied glyph (markerBitmap/markerAsset) at the left edge
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
  // Inherit sentinels: Screen::list() substitutes the theme value for
  // rowHeight <= 0, rowGap < 0, sidePadding < 0, and rowRadius == 0; raw
  // list() falls back to 36 / 0 / 8. Literal defaults here would silently
  // override the theme for every Screen::list() caller that leaves them
  // unset.
  int16_t rowHeight = 0;
  int16_t rowGap = -1;
  uint8_t rowRadius = 0;
  int16_t sidePadding = -1;
  int16_t textGap = 10;
  int16_t iconSize = 0;
  // Extra inset for the right-aligned value slot beyond sidePadding, so a
  // trailing chevron/value keeps air from the row edge on themes with tight
  // row padding.
  int16_t valueInset = 0;
  // Switch geometry for ListItem::toggle rows (mirrors ToggleRowProps).
  // Colors derive from the row style's foreground so the switch stays legible
  // on inverted (selected) rows.
  int16_t toggleWidth = 38;
  int16_t toggleHeight = 18;
  uint8_t toggleRadius = 0;
  uint8_t toggleKnobRadius = 0;
  int16_t toggleKnobInset = 3;
  uint8_t toggleBorderWidth = 1;
  // Horizontal inset of the ROWS within the rect (the Lyra pill band). The
  // scroll indicator stays at the rect's edge, in the inset margin.
  // -1 = inherit: Screen::list() substitutes the theme's listInset.
  int16_t rowInset = -1;
  // Inherit sentinels like rowGap/sidePadding: width -1 = theme (raw list()
  // falls back to 3); side 0xFF = theme (raw falls back to right).
  int16_t scrollIndicatorWidth = -1;
  uint8_t scrollIndicatorSide = 0xFF;  // 0 = right edge, 1 = left edge
  bool centerSingleLine = false;
  // Shrink each row's background/hit area to its label width plus side
  // padding instead of the full rect width (hug-content menu rows).
  bool hugContents = false;
  // Draws a thin position indicator along the right edge when the list
  // overflows the rect.
  bool scrollIndicator = true;
  // Draw a non-interactive preview of the next row when there is
  // leftover space after the fully visible rows. Scroll math still uses only
  // full rows so paging stays deterministic.
  bool partialTrailingRow = false;
  int16_t partialTrailingMinHeight = 18;
  // Additional marker drawn on the selected row (the v1 theme Underline and
  // Triangle selection styles).
  SelectionMarker selectionMarker = SelectionMarker::None;
  Paint markerPaint = Paint::solid(Color::Black);
  int16_t markerInset = 0;      // x offset of the marker / underline start
  int16_t markerThickness = 2;  // underline thickness
  // Glyph for SelectionMarker::Bitmap, drawn vertically centered at the
  // selected row's left edge (markerInset offset). Direct bitmap wins;
  // otherwise the asset resolves through the frame's AssetResolver.
  BitmapRef markerBitmap{};
  AssetRef markerAsset{};
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
  const int16_t rowGap = props.rowGap < 0 ? 0 : props.rowGap;
  const int16_t sidePad = props.sidePadding < 0 ? 8 : props.sidePadding;
  const int16_t scrollW = props.scrollIndicatorWidth < 0 ? 3 : props.scrollIndicatorWidth;
  const bool scrollLeft = props.scrollIndicatorSide == 1;
  const int16_t rowInset = props.rowInset < 0 ? 0 : props.rowInset;
  const uint16_t visible = listVisibleRows(rect, rowH, rowGap);
  const bool overflows = props.count > visible;
  uint16_t top = props.topIndex;
  if (top > props.count - 1) top = props.count - 1;
  if (overflows && top > props.count - visible) top = static_cast<uint16_t>(props.count - visible);
  if (!overflows) top = 0;
  const uint16_t end = overflows ? static_cast<uint16_t>(top + visible) : props.count;

  Rect rowArea = rect;
  if (rowInset > 0) {
    rowArea.x = static_cast<int16_t>(rowArea.x + rowInset);
    rowArea.width = static_cast<int16_t>(rowArea.width - rowInset * 2);
  }
  if (props.scrollIndicator && overflows && scrollW > 0) {
    // Rows only give up width when the row inset margin doesn't already
    // clear the track plus 2px of air.
    const int16_t needed = static_cast<int16_t>(scrollW + 2);
    if (rowInset < needed) {
      const int16_t cut = static_cast<int16_t>(needed - rowInset);
      rowArea.width = static_cast<int16_t>(rowArea.width - cut);
      if (scrollLeft) rowArea.x = static_cast<int16_t>(rowArea.x + cut);
    }
    Rect track{scrollLeft ? rect.x : static_cast<int16_t>(rect.right() - scrollW), rect.y, scrollW, rect.height};
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
      Rect headerRow{static_cast<int16_t>(rowArea.x + sidePad), cursorY,
                     static_cast<int16_t>(rowArea.width - sidePad * 2), headerLh};
      frame.target().text(headerRow, item.label, props.headerText);
      if (props.headerUnderline) {
        frame.target().fill(Rect{headerRow.x, static_cast<int16_t>(cursorY + headerLh + 2), headerRow.width, 1},
                            Paint::solid(props.headerText.color));
      }
      cursorY = static_cast<int16_t>(cursorY + headerH + rowGap);
      continue;
    }
    if (static_cast<int16_t>(cursorY + rowH) > rowArea.bottom() || drawnRows >= visible || i >= end) break;
    ++drawnRows;
    Rect row{rowArea.x, cursorY, rowArea.width, rowH};
    cursorY = static_cast<int16_t>(cursorY + rowH + rowGap);
    if (props.hugContents && item.label) {
      // Hug-content rows shrink to the label width plus padding so the
      // selection pill wraps the text instead of spanning the rect.
      const int16_t labelW = frame.target().measureText(props.labelText.font, item.label, props.labelText).width;
      const int16_t hugW = static_cast<int16_t>(labelW + sidePad * 2);
      if (hugW < row.width) row.width = hugW;
    }
    State state = item.state;
    if (props.selectedIndex == static_cast<int16_t>(i)) state |= StateSelected;
    if (!item.enabled) state |= StateDisabled;
    if (props.action != NO_ACTION && item.enabled) {
      // item.enabled controls interactivity; a StateDisabled carried in
      // item.state is visual-only dimming and must not block touch routing
      // (findTouch skips disabled interactions).
      const State hitState = static_cast<State>(static_cast<int>(state) & ~static_cast<int>(StateDisabled));
      frame.hit(ensureMinTouchRect(row, frame.device().minTouchSize, frame.screen()), props.action, item.actionValue,
                props.inputMask, hitState);
    }
    state = frame.stateFor(props.action, item.actionValue, state);
    StyleSet styles = props.rowStyles.unset() ? defaultListRowStyles() : props.rowStyles;
    if (props.rowRadius > 0) setStyleRadius(styles, props.rowRadius);
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(row, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(row, style.border, style.borderWidth, style.radius, style.corners);
    }

    Rect content = row.inset(Insets{0, sidePad, 0, sidePad});

    // Slot layout (mirrors settingRow): the label owns a "title band" and the
    // icon and value align to it; the subtitle spans the full content width
    // under the band so it never collides with the value.
    TextStyle labelStyle = textStyleWithForeground(props.labelText, style.foreground);
    const int16_t labelH = frame.target().lineHeight(labelStyle.font);
    const int16_t subH = item.subtitle ? frame.target().lineHeight(props.subtitleText.font) : 0;
    Rect band = content;
    if (item.subtitle) {
      int16_t bandTop = static_cast<int16_t>(content.y + (content.height - labelH - subH) / 2);
      if (bandTop < content.y) bandTop = content.y;
      band = Rect{content.x, bandTop, content.width, labelH};
    }

    const BitmapRef icon = item.icon ? item.icon : resolveBitmap(frame.assets(), item.iconAsset);
    if (icon) {
      const int16_t iconSize = props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon.width);
      // Centered on the full row content, not the title band: with a subtitle
      // the icon belongs to the label+subtitle block as a whole.
      Rect iconRect{content.x, static_cast<int16_t>(content.y + (content.height - iconSize) / 2), iconSize, iconSize};
      frame.target().bitmap(iconRect, icon, BitmapMode::Contain, style.foreground);
      content.x = static_cast<int16_t>(content.x + iconSize + props.textGap);
      content.width = static_cast<int16_t>(content.width - iconSize - props.textGap);
      band.x = content.x;
      band.width = content.width;
    }

    int16_t availW = band.width;
    if (item.toggle) {
      const int16_t togW = props.toggleWidth < 18 ? 18 : props.toggleWidth;
      const int16_t togH = props.toggleHeight < 12 ? 12 : props.toggleHeight;
      Rect toggleRect{static_cast<int16_t>(band.x + band.width - togW - props.valueInset),
                      static_cast<int16_t>(band.y + (band.height - togH) / 2), togW, togH};
      // The switch draws in row-foreground ink with the foreground's opposite
      // as "paper", so it inverts along with the row when selected.
      const Paint fg = style.foreground;
      const bool fgWhite = fg.kind == PaintKind::Solid && fg.color == Color::White;
      const Paint paper = Paint::solid(fgWhite ? Color::Black : Color::White);
      const uint8_t trackRadius = static_cast<uint8_t>(props.toggleRadius > togH / 2 ? togH / 2 : props.toggleRadius);
      frame.target().fill(toggleRect, item.toggleChecked ? fg : paper, trackRadius);
      if (props.toggleBorderWidth > 0) {
        frame.target().stroke(toggleRect, fg, props.toggleBorderWidth, trackRadius);
      }
      const int16_t knobInset = props.toggleKnobInset < 0 ? 0 : props.toggleKnobInset;
      const int16_t knobH = static_cast<int16_t>(togH - knobInset * 2);
      if (knobH > 0) {
        Rect knob{static_cast<int16_t>(item.toggleChecked ? toggleRect.right() - knobInset - knobH
                                                          : toggleRect.x + knobInset),
                  static_cast<int16_t>(toggleRect.y + knobInset), knobH, knobH};
        const uint8_t knobRadius =
            static_cast<uint8_t>(props.toggleKnobRadius > knobH / 2 ? knobH / 2 : props.toggleKnobRadius);
        frame.target().fill(knob, item.toggleChecked ? paper : fg, knobRadius);
      }
      availW = static_cast<int16_t>(availW - togW - props.valueInset - props.textGap);
    } else if (item.value) {
      TextStyle valueStyle = textStyleWithForeground(props.valueText, style.foreground);
      valueStyle.align = TextAlign::Right;
      const int16_t valueW = frame.target().measureText(valueStyle.font, item.value, valueStyle).width;
      Rect valueRect{static_cast<int16_t>(band.x + availW - valueW - props.valueInset), band.y, valueW, band.height};
      frame.target().text(valueRect, item.value, valueStyle);
      availW = static_cast<int16_t>(availW - valueW - props.valueInset - props.textGap);
    }

    if (labelStyle.maxLines > 1 && (item.toggle || item.value) && item.label) {
      // A label that fits stays on one line; one that must wrap breaks early
      // (60% of the band) for a balanced two-line split instead of running
      // right up against the trailing slot.
      const int16_t labelW = frame.target().measureText(labelStyle.font, item.label, labelStyle).width;
      if (labelW > availW) {
        const int16_t wrapCap = static_cast<int16_t>((band.width * 3) / 5);
        if (availW > wrapCap) availW = wrapCap;
      }
    }

    if (item.subtitle) {
      frame.target().text(Rect{band.x, band.y, availW, band.height}, item.label, labelStyle);
      frame.target().text(Rect{content.x, static_cast<int16_t>(band.y + labelH), content.width, subH},
                          item.subtitle, textStyleWithForeground(props.subtitleText, style.foreground));
    } else {
      if (props.centerSingleLine) labelStyle.align = TextAlign::Center;
      frame.target().text(Rect{band.x, band.y, availW, band.height}, item.label, labelStyle);
    }

    if (props.selectedIndex == static_cast<int16_t>(i) && props.selectionMarker != SelectionMarker::None) {
      if (props.selectionMarker == SelectionMarker::Underline) {
        frame.target().fill(Rect{static_cast<int16_t>(row.x + sidePad + props.markerInset),
                                 static_cast<int16_t>(row.bottom() - props.markerThickness),
                                 static_cast<int16_t>(row.width - sidePad * 2 - props.markerInset),
                                 props.markerThickness},
                            props.markerPaint);
      } else if (props.selectionMarker == SelectionMarker::Bitmap) {
        const BitmapRef marker =
            props.markerBitmap ? props.markerBitmap : resolveBitmap(frame.assets(), props.markerAsset);
        if (marker) {
          frame.target().bitmap(Rect{static_cast<int16_t>(row.x + props.markerInset),
                                     static_cast<int16_t>(row.y + (row.height - marker.height) / 2),
                                     static_cast<int16_t>(marker.width), static_cast<int16_t>(marker.height)},
                                marker, BitmapMode::Contain, props.markerPaint);
        }
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

  if (props.partialTrailingRow && overflows && visible > 0) {
    const uint16_t partialIndex = static_cast<uint16_t>(top + visible);
    const int16_t remainingH = static_cast<int16_t>(rowArea.bottom() - cursorY);
    if (partialIndex < props.count && remainingH >= props.partialTrailingMinHeight) {
      const ListItem& item = props.items[partialIndex];
      if (!item.isHeader && item.label != nullptr && item.label[0] != '\0') {
        Rect row{rowArea.x, cursorY, rowArea.width, remainingH};
        StyleSet styles = props.rowStyles.unset() ? defaultListRowStyles() : props.rowStyles;
        if (props.rowRadius > 0) setStyleRadius(styles, props.rowRadius);
        const BoxStyle& style = styles.resolve(item.enabled ? StateNormal : StateDisabled);
        frame.target().fill(row, style.background, style.radius, style.corners);
        if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
          frame.target().stroke(row, style.border, style.borderWidth, style.radius, style.corners);
        }

        Rect content = row.inset(Insets{0, sidePad, 0, sidePad});
        TextStyle labelStyle = textStyleWithForeground(props.labelText, style.foreground);
        labelStyle.maxLines = 1;
        TextStyle subtitleStyle = textStyleWithForeground(props.subtitleText, style.foreground);
        subtitleStyle.maxLines = 1;
        const int16_t labelH = frame.target().lineHeight(labelStyle.font);
        const int16_t subH = item.subtitle ? frame.target().lineHeight(subtitleStyle.font) : 0;
        const int16_t textBlockH = static_cast<int16_t>(labelH + subH);
        int16_t textY = content.y;
        if (content.height > textBlockH) {
          textY = static_cast<int16_t>(content.y + (content.height - textBlockH) / 2);
        }
        Rect band{content.x, textY, content.width, labelH};
        const BitmapRef icon = item.icon ? item.icon : resolveBitmap(frame.assets(), item.iconAsset);
        if (icon) {
          const int16_t iconSize = props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon.width);
          Rect iconRect{content.x, static_cast<int16_t>(band.y + (band.height - iconSize) / 2), iconSize, iconSize};
          frame.target().bitmap(iconRect, icon, BitmapMode::Contain, style.foreground);
          content.x = static_cast<int16_t>(content.x + iconSize + props.textGap);
          content.width = static_cast<int16_t>(content.width - iconSize - props.textGap);
          band.x = content.x;
          band.width = content.width;
        }
        frame.target().text(band, item.label, labelStyle);
        if (item.subtitle && subH > 0) {
          frame.target().text(Rect{content.x, static_cast<int16_t>(band.y + labelH), content.width, subH},
                              item.subtitle, subtitleStyle);
        }
      }
    }
  }
}

}  // namespace ui
}  // namespace freeink
