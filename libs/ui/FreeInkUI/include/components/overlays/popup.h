#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/progress-bar.h"

namespace freeink {
namespace ui {

struct PopupProps {
  const char* message = nullptr;
  TextStyle text{};
  StyleSet styles{};
  Insets padding{12, 16, 12, 16};
  int16_t maxWidth = 0;  // 0 = 3/4 bounds width when auto-sized by Screen::popup.
  ProgressBarProps progress{};
  int16_t progressHeight = 4;
  bool showProgress = false;
};

template <size_t MaxInteractions>
void popup(Frame<MaxInteractions>& frame, Rect rect, const PopupProps& props) {
  StyleSet styles = props.styles.unset() ? defaultPopupStyles() : props.styles;
  const BoxStyle& style = styles.resolve(StateNormal);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }
  Rect content = rect.inset(props.padding);
  if (props.showProgress) {
    const int16_t barH = props.progressHeight > 0 ? props.progressHeight : 4;
    Rect msg{content.x, content.y, content.width, static_cast<int16_t>(content.height - barH - 6)};
    frame.target().text(msg, props.message, props.text);
    progressBar(frame, Rect{content.x, static_cast<int16_t>(content.bottom() - barH), content.width, barH},
                props.progress);
  } else {
    frame.target().text(content, props.message, props.text);
  }
}

}  // namespace ui
}  // namespace freeink
