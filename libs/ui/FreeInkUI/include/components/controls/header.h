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
  int16_t minTouchSize = 44;
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

  Rect content = rect.inset(Insets{0, 6, 0, 6});

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
    button(frame, Rect{static_cast<int16_t>(rect.x + 4), static_cast<int16_t>(rect.y + 4), btn, btn}, back);
    // A non-centered title starts after the button; a centered one keeps the
    // full band so it lines up across screens with and without a back button.
    if (!props.centered) {
      const int16_t used = static_cast<int16_t>(btn + 8);
      content.x = static_cast<int16_t>(content.x + used);
      content.width = static_cast<int16_t>(content.width - used);
    }
  }

  TextStyle titleStyle = props.titleText;
  titleStyle.align = props.centered ? TextAlign::Center : TextAlign::Left;
  if (props.title) {
    int16_t titleY = static_cast<int16_t>(content.y + props.titleOffsetY);
    if (leading && props.leadingAction != NO_ACTION && !props.subtitle) {
      // With a leading button and a single line, center the title vertically.
      const int16_t lh = frame.target().lineHeight(titleStyle.font);
      titleY = static_cast<int16_t>(rect.y + (rect.height - lh) / 2 + props.titleOffsetY);
    }
    Rect titleRect{content.x, titleY, content.width, content.height};
    if (props.rightLabel) {
      const Size rightSize = frame.target().measureText(props.subtitleText.font, props.rightLabel, props.subtitleText);
      Rect rightRect{static_cast<int16_t>(content.right() - rightSize.width), content.y, rightSize.width,
                     content.height};
      frame.target().text(rightRect, props.rightLabel, props.subtitleText);
      titleRect.width = static_cast<int16_t>(titleRect.width - rightSize.width - 6);
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
