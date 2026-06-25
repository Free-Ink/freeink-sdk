#pragma once

#include "../../FreeInkUICore.h"
#include "../overlays/popup.h"

namespace freeink {
namespace ui {

enum class ToastAnchor : uint8_t {
  Top,
  Center,
  Bottom,
};

struct ToastProps {
  const char* message = nullptr;
  TextStyle text{};
  StyleSet styles{};
  Insets padding{8, 12, 8, 12};
  ToastAnchor anchor = ToastAnchor::Bottom;
  int16_t maxWidth = 0;  // 0 = 3/4 screen width
  int16_t margin = 16;
};

template <size_t MaxInteractions>
void toast(Frame<MaxInteractions>& frame, Rect bounds, const ToastProps& props) {
  if (!props.message) return;
  const int16_t maxW = props.maxWidth > 0 ? props.maxWidth : static_cast<int16_t>(bounds.width * 3 / 4);
  TextStyle text = props.text;
  text.align = TextAlign::Center;
  const Size textSize = measureWrappedText(frame.target(), props.message, text,
                                           static_cast<int16_t>(maxW - props.padding.left - props.padding.right));
  const Size panelSize{static_cast<int16_t>(textSize.width + props.padding.left + props.padding.right),
                       static_cast<int16_t>(textSize.height + props.padding.top + props.padding.bottom)};
  int16_t y = bounds.y;
  if (props.anchor == ToastAnchor::Center) {
    y = static_cast<int16_t>(bounds.y + (bounds.height - panelSize.height) / 2);
  } else if (props.anchor == ToastAnchor::Bottom) {
    y = static_cast<int16_t>(bounds.bottom() - panelSize.height - props.margin);
  } else {
    y = static_cast<int16_t>(bounds.y + props.margin);
  }
  Rect panel{static_cast<int16_t>(bounds.x + (bounds.width - panelSize.width) / 2), y, panelSize.width,
             panelSize.height};
  PopupProps popupProps;
  popupProps.message = props.message;
  popupProps.text = text;
  popupProps.styles = props.styles;
  popupProps.padding = props.padding;
  popup(frame, panel, popupProps);
}

}  // namespace ui
}  // namespace freeink
