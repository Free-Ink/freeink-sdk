#pragma once

#include "../../FreeInkUICore.h"
#include "../bars/battery-indicator.h"
#include "button.h"

namespace freeink {
namespace ui {

// Passive status chrome for the header band: a battery glyph hugging one edge
// and an optional clock hugging the other, both anchored to the band's top
// strip. header() derives the title reserves from these so text never runs
// under either. The app owns all strings (percent label, clock text) — they
// must stay alive until header() returns.
struct HeaderStatusProps {
  bool showBattery = false;
  BatteryIndicatorProps battery{};
  bool batteryLeft = false;
  int16_t stripHeight = 0;  // status strip height; set it when status is shown
  int16_t edgeInset = -1;   // -1 = the resolved sidePadding
  const char* clockText = nullptr;
  // Center the clock on the band (both axes) instead of anchoring it to the
  // corner opposite the battery. A non-centered title truncates before it.
  bool clockCentered = false;
};

struct HeaderProps {
  const char* title = nullptr;
  const char* subtitle = nullptr;
  const char* rightLabel = nullptr;
  TextStyle titleText{};
  TextStyle subtitleText{};
  StyleSet styles{};
  int16_t titleOffsetY = 0;
  uint8_t borderEdges = EdgesAll;
  bool centered = false;
  // Leading action button (the "back + centered title" chrome every
  // multi-screen app needs). Set an icon and an action to get a square button
  // on the left edge; with `centered` the title stays centered on the full
  // band and is vertically centered when there is no subtitle.
  BitmapRef leadingIcon{};
  AssetRef leadingIconAsset{};
  ActionId leadingAction = NO_ACTION;
  int16_t leadingValue = 0;
  StyleSet leadingStyles{};
  uint8_t leadingRadius = 0;
  // Trailing action button on the right edge (a "Save"/"Done" mirroring the
  // leading back button). Label, icon, or both. Occupies the same slot as
  // rightLabel — set one or the other.
  const char* trailingLabel = nullptr;
  BitmapRef trailingIcon{};
  AssetRef trailingIconAsset{};
  ActionId trailingAction = NO_ACTION;
  int16_t trailingValue = 0;
  StyleSet trailingStyles{};
  uint8_t trailingRadius = 0;
  // Optional second trailing action. It sits directly to the left of the
  // primary trailing button, keeping related header actions together.
  BitmapRef trailingAdjacentIcon{};
  ActionId trailingAdjacentAction = NO_ACTION;
  int16_t trailingAdjacentValue = 0;
  TextStyle trailingText{};
  bool trailingEnabled = true;
  int16_t minTouchSize = 44;
  // Left/right inset of the text content (title, subtitle, rightLabel). Set
  // it to the screen's list/content padding so the title's left edge lines up
  // with the rows below. Leading/trailing buttons stay anchored to the band
  // edges regardless. -1 = inherit: Screen::header() substitutes the theme's
  // headerSidePadding; raw header() falls back to 6.
  int16_t sidePadding = -1;
  // Vertical nudge for the leading/trailing action buttons. Text centers on
  // its font's line cell and the glyphs sit below the cell's geometric
  // center by the font's internal leading; icons center on their exact pixel
  // box. Apps can pass ~(lineHeight - ascender) / 2 of the title font so
  // icon buttons optically align with the title glyphs.
  int16_t actionOffsetY = 0;
  // Extra width reserved at the content's left/right edge for app-drawn
  // extras: text truncates before it, but the band's background and border
  // still span the full rect. Not meant to combine with a leading/trailing
  // action button on the same edge. Battery/clock chrome does not need this —
  // set `status` and the reserves are derived automatically.
  int16_t leftReserve = 0;
  int16_t rightReserve = 0;
  // Caps for the action buttons' squares (0 = band height - 8). Tall two-row
  // bands set these so the buttons match the standard band's size and sit on
  // the title line via actionOffsetY instead of filling the band.
  int16_t leadingSize = 0;
  int16_t trailingSize = 0;
  // Battery + clock status chrome drawn inside the band.
  HeaderStatusProps status{};
};

template <size_t MaxInteractions>
void header(Frame<MaxInteractions>& frame, Rect rect, const HeaderProps& props) {
  StyleSet styles = props.styles.unset() ? defaultPopupStyles() : props.styles;
  const BoxStyle& style = styles.resolve(StateNormal);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    drawBorderEdges(frame.target(), rect, style.border, style.borderWidth, style.radius, style.corners,
                    props.borderEdges);
  }

  const int16_t sidePad = props.sidePadding < 0 ? 6 : props.sidePadding;
  Rect content = rect.inset(Insets{0, sidePad, 0, sidePad});
  if (props.leftReserve > 0) {
    content.x = static_cast<int16_t>(content.x + props.leftReserve);
    content.width = static_cast<int16_t>(content.width - props.leftReserve);
  }
  if (props.rightReserve > 0) content.width = static_cast<int16_t>(content.width - props.rightReserve);

  // A title centered by its text style must keep the full band like the
  // navHeader `centered` flag, or the leading/trailing insets below would
  // shift its centering off the band's middle.
  const bool centeredTitle = props.centered || props.titleText.align == TextAlign::Center;

  // Status chrome measurements first: on shared-line layouts the battery and
  // clock reserve title space on their respective sides. (+2: the battery
  // glyph's terminal nub extends past glyphWidth.)
  const HeaderStatusProps& status = props.status;
  const int16_t statusStripH = status.stripHeight;
  const int16_t statusInset = status.edgeInset < 0 ? sidePad : status.edgeInset;
  int16_t batteryReserve = 0;
  if (status.showBattery) {
    batteryReserve = static_cast<int16_t>(status.battery.glyphWidth + 2);
    if (status.battery.label) {
      batteryReserve = static_cast<int16_t>(
          batteryReserve + status.battery.gap +
          frame.target().measureText(status.battery.text.font, status.battery.label, status.battery.text).width);
    }
  }
  int16_t clockWidth = 0;
  if (status.clockText) {
    clockWidth = frame.target().measureText(status.battery.text.font, status.clockText, status.battery.text).width;
  }
  // Reserves are only needed while the title's ink actually shares the status
  // strip: on tall bands the centered title sits below the strip and keeps its
  // full width under the corner chrome.
  const int16_t titleLineH = frame.target().lineHeight(props.titleText.font);
  const bool titleSharesStrip =
      static_cast<int16_t>((rect.height - titleLineH) / 2 + props.titleOffsetY) < statusStripH;
  if (titleSharesStrip && status.showBattery) {
    const int16_t reserve = static_cast<int16_t>(batteryReserve + 8);
    if (status.batteryLeft) {
      content.x = static_cast<int16_t>(content.x + reserve);
      content.width = static_cast<int16_t>(content.width - reserve);
    } else {
      content.width = static_cast<int16_t>(content.width - reserve);
    }
  }
  if (clockWidth > 0 && status.clockCentered && !centeredTitle && titleSharesStrip) {
    // A left-anchored title on the strip line stops short of the centered
    // clock; a title below the strip keeps its width.
    const int16_t clockStart = static_cast<int16_t>(rect.x + (rect.width - clockWidth) / 2);
    const int16_t maxWidth = static_cast<int16_t>(clockStart - 8 - content.x);
    if (maxWidth < content.width) content.width = maxWidth;
  }

  const BitmapRef leading = props.leadingIcon ? props.leadingIcon : resolveBitmap(frame.assets(), props.leadingIconAsset);
  if (leading && props.leadingAction != NO_ACTION) {
    const int16_t btn =
        props.leadingSize > 0 ? props.leadingSize : static_cast<int16_t>(rect.height - 8);
    ButtonProps back;
    back.icon = leading;
    back.action = props.leadingAction;
    back.value = props.leadingValue;
    back.styles = props.leadingStyles;
    back.radius = props.leadingRadius;
    back.minTouchSize = props.minTouchSize;
    button(frame,
           Rect{static_cast<int16_t>(rect.x + 4), static_cast<int16_t>(rect.y + 4 + props.actionOffsetY), btn, btn},
           back);
    // The button replaces the side padding at the leading edge rather than
    // stacking on top of it, and a non-centered title tucks in just right of
    // the icon's ink: the icon centers in the square, so anchoring on the
    // (usually invisible) button box would leave (btn - icon)/2 of dead
    // space. A centered title keeps the full band so it lines up across
    // screens with and without a back button.
    if (!centeredTitle) {
      const int16_t iconEnd = static_cast<int16_t>(4 + (btn + leading.width) / 2);
      const int16_t inset = static_cast<int16_t>(iconEnd + 6 - sidePad);
      if (inset > 0) {
        content.x = static_cast<int16_t>(content.x + inset);
        content.width = static_cast<int16_t>(content.width - inset);
      }
    }
  }
  const BitmapRef trailing =
      props.trailingIcon ? props.trailingIcon : resolveBitmap(frame.assets(), props.trailingIconAsset);
  if ((props.trailingLabel || trailing) && props.trailingAction != NO_ACTION) {
    const int16_t btnH =
        props.trailingSize > 0 ? props.trailingSize : static_cast<int16_t>(rect.height - 8);
    int16_t btnW = btnH;   // icon-only: square, like the leading button
    if (props.trailingLabel) {
      const Size labelSize =
          frame.target().measureText(props.trailingText.font, props.trailingLabel, props.trailingText);
      btnW = static_cast<int16_t>(labelSize.width + 20 + (trailing ? trailing.width + 4 : 0));
    }
    // The battery sits in the corner headroom above this row, so the
    // trailing buttons keep the right edge.
    const int16_t trailingAnchor = static_cast<int16_t>(rect.right() - 12);
    const int16_t trailingY = static_cast<int16_t>(rect.y + 4 + props.actionOffsetY);
    ButtonProps action;
    action.label = props.trailingLabel;
    action.icon = trailing;
    action.action = props.trailingAction;
    action.value = props.trailingValue;
    action.styles = props.trailingStyles;
    action.radius = props.trailingRadius;
    action.text = props.trailingText;
    action.enabled = props.trailingEnabled;
    action.minTouchSize = props.minTouchSize;
    button(frame, Rect{static_cast<int16_t>(trailingAnchor - btnW), trailingY, btnW, btnH}, action);
    const bool hasAdjacentTrailing = props.trailingAdjacentIcon && props.trailingAdjacentAction != NO_ACTION;
    if (hasAdjacentTrailing) {
      ButtonProps adjacent;
      adjacent.icon = props.trailingAdjacentIcon;
      adjacent.action = props.trailingAdjacentAction;
      adjacent.value = props.trailingAdjacentValue;
      adjacent.styles = props.trailingStyles;
      adjacent.radius = props.trailingRadius;
      adjacent.minTouchSize = props.minTouchSize;
      button(frame, Rect{static_cast<int16_t>(trailingAnchor - 4 - btnW - btnH), trailingY, btnH, btnH}, adjacent);
    }
    if (!centeredTitle) {
      content.width = static_cast<int16_t>(content.width - btnW - (hasAdjacentTrailing ? btnH + 4 : 0) - 8);
    }
  }

  // Alignment comes from the title style (themes can left/center/right
  // align); `centered` remains the navHeader convenience override.
  TextStyle titleStyle = props.titleText;
  if (props.centered) titleStyle.align = TextAlign::Center;
  if (props.title) {
    int16_t titleY = static_cast<int16_t>(content.y + props.titleOffsetY);
    Rect titleRect{content.x, titleY, content.width, content.height};
    if (props.rightLabel) {
      const Size rightSize = frame.target().measureText(props.subtitleText.font, props.rightLabel, props.subtitleText);
      // Bottom-aligned to the title's line (including its vertical offset):
      // a smaller label centered on the band would float above the title.
      const int16_t titleLh = frame.target().lineHeight(props.titleText.font);
      const int16_t titleTop =
          static_cast<int16_t>(content.y + props.titleOffsetY + (content.height - titleLh) / 2);
      Rect rightRect{static_cast<int16_t>(content.right() - rightSize.width),
                     static_cast<int16_t>(titleTop + titleLh - rightSize.height), rightSize.width, rightSize.height};
      frame.target().text(rightRect, props.rightLabel, props.subtitleText);
      // Reserve the label's width out of the title rect. A centered title
      // gives up the same width on both sides so it stays centered on the
      // band; a left-aligned one only loses the right slice.
      const int16_t used = static_cast<int16_t>(rightSize.width + 6);
      titleRect.width = static_cast<int16_t>(titleRect.width - used);
      if (centeredTitle) {
        titleRect.x = static_cast<int16_t>(titleRect.x + used);
        titleRect.width = static_cast<int16_t>(titleRect.width - used);
      }
    }
    frame.target().text(titleRect, props.title, titleStyle);
  }
  if (props.subtitle) {
    Rect subRect{content.x, static_cast<int16_t>(content.y + frame.target().lineHeight(props.titleText.font)),
                 content.width, static_cast<int16_t>(content.height - frame.target().lineHeight(props.titleText.font))};
    frame.target().text(subRect, props.subtitle, props.subtitleText);
  }

  // The status strip is reserved chrome: the battery pins to its corner and
  // the clock to its slot, and neither ever repositions for other header
  // content. Apps place action buttons below the strip via actionOffsetY.
  if (status.showBattery) {
    const int16_t batteryX = status.batteryLeft
                                 ? static_cast<int16_t>(rect.x + statusInset)
                                 : static_cast<int16_t>(rect.right() - statusInset - batteryReserve);
    batteryIndicator(frame, Rect{batteryX, rect.y, batteryReserve, statusStripH}, status.battery);
  }
  if (clockWidth > 0) {
    // Centered: on the band's midline with the rest of the chrome. Corner:
    // in the headroom above the title row, top-aligned like the legacy
    // status strips (its own line height, not the battery strip).
    const int16_t clockLineH = frame.target().lineHeight(status.battery.text.font);
    const int16_t clockH = status.clockCentered ? statusStripH : clockLineH;
    const int16_t clockX = status.clockCentered
                               ? static_cast<int16_t>(rect.x + (rect.width - clockWidth) / 2)
                           : status.batteryLeft
                               ? static_cast<int16_t>(rect.right() - statusInset - clockWidth)
                               : static_cast<int16_t>(rect.x + statusInset);
    frame.target().text(Rect{clockX, rect.y, clockWidth, clockH}, status.clockText, status.battery.text);
  }
}

}  // namespace ui
}  // namespace freeink
