#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/progress-bar.h"
#include "../controls/button.h"

namespace freeink {
namespace ui {

struct MessagePanelProps {
  const char* title = nullptr;
  const char* message = nullptr;
  const char* actionLabel = nullptr;
  ActionId action = NO_ACTION;
  int16_t actionValue = 0;
  TextStyle titleText{};
  TextStyle messageText{};
  TextStyle buttonText{};
  StyleSet panelStyles{};
  StyleSet buttonStyles{};
  Insets padding{16, 20, 16, 20};
  int16_t gap = 8;
  int16_t buttonHeight = 40;
  bool showProgress = false;
  ProgressBarProps progress{};
};

template <size_t MaxInteractions>
void messagePanel(Frame<MaxInteractions>& frame, Rect rect, const MessagePanelProps& props) {
  StyleSet styles = props.panelStyles.unset() ? defaultPopupStyles() : props.panelStyles;
  const BoxStyle& style = styles.resolve(StateNormal);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  int16_t buttonH = props.actionLabel ? props.buttonHeight : 0;
  int16_t progressH = props.showProgress ? 4 : 0;
  int16_t y = content.y;
  if (props.title) {
    const int16_t titleH = frame.target().lineHeight(props.titleText.font);
    TextStyle title = props.titleText;
    title.align = TextAlign::Center;
    frame.target().text(Rect{content.x, y, content.width, titleH}, props.title, title);
    y = static_cast<int16_t>(y + titleH + props.gap);
  }
  if (props.message) {
    TextStyle msg = props.messageText;
    msg.align = TextAlign::Center;
    Rect msgRect{content.x, y, content.width,
                 static_cast<int16_t>(content.bottom() - y - buttonH - progressH - props.gap * 2)};
    frame.target().text(msgRect, props.message, msg);
  }
  if (props.showProgress) {
    progressBar(frame, Rect{content.x, static_cast<int16_t>(content.bottom() - buttonH - props.gap - 4),
                            content.width, 4},
                props.progress);
  }
  if (props.actionLabel) {
    ButtonProps bp;
    bp.label = props.actionLabel;
    bp.action = props.action;
    bp.value = props.actionValue;
    bp.text = props.buttonText;
    bp.styles = props.buttonStyles;
    bp.minTouchSize = frame.device().minTouchSize;
    button(frame, Rect{content.x, static_cast<int16_t>(content.bottom() - buttonH), content.width, buttonH}, bp);
  }
}

}  // namespace ui
}  // namespace freeink
