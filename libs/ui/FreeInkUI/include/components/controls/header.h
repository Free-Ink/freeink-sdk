#pragma once

#include "../../FreeInkUICore.h"
#include "button.h"

namespace freeink {
namespace ui {

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
  // extras (e.g. a battery indicator): text truncates before it, but the
  // band's background and border still span the full rect. Not meant to
  // combine with a leading/trailing action button on the same edge.
  int16_t leftReserve = 0;
  int16_t rightReserve = 0;
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

  const BitmapRef leading = props.leadingIcon ? props.leadingIcon : resolveBitmap(frame.assets(), props.leadingIconAsset);
  if (leading && props.leadingAction != NO_ACTION) {
    const int16_t btn = static_cast<int16_t>(rect.height - 8);
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
    // A non-centered title starts after the button; a centered one keeps the
    // full band so it lines up across screens with and without a back button.
    if (!props.centered) {
      const int16_t used = static_cast<int16_t>(btn + 8);
      content.x = static_cast<int16_t>(content.x + used);
      content.width = static_cast<int16_t>(content.width - used);
    }
  }

  const BitmapRef trailing =
      props.trailingIcon ? props.trailingIcon : resolveBitmap(frame.assets(), props.trailingIconAsset);
  if ((props.trailingLabel || trailing) && props.trailingAction != NO_ACTION) {
    const int16_t btnH = static_cast<int16_t>(rect.height - 8);
    int16_t btnW = btnH;   // icon-only: square, like the leading button
    if (props.trailingLabel) {
      const Size labelSize =
          frame.target().measureText(props.trailingText.font, props.trailingLabel, props.trailingText);
      btnW = static_cast<int16_t>(labelSize.width + 20 + (trailing ? trailing.width + 4 : 0));
    }
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
    button(frame,
           Rect{static_cast<int16_t>(rect.right() - 4 - btnW), static_cast<int16_t>(rect.y + 4 + props.actionOffsetY),
                btnW, btnH},
           action);
    if (!props.centered) {
      content.width = static_cast<int16_t>(content.width - btnW - 8);
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
      if (props.centered) {
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
}

}  // namespace ui
}  // namespace freeink
