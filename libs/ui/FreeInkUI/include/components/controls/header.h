#pragma once

#include "../../FreeInkUICore.h"

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
  TextStyle titleStyle = props.titleText;
  titleStyle.align = props.centered ? TextAlign::Center : TextAlign::Left;
  if (props.title) {
    Rect titleRect{content.x, static_cast<int16_t>(content.y + props.titleOffsetY), content.width, content.height};
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
