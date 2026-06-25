#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/button.h"
#include "../overlays/option-dialog.h"

namespace freeink {
namespace ui {

struct ContextMenuProps {
  const char* title = nullptr;
  const DialogOption* options = nullptr;
  uint8_t optionCount = 0;
  TextStyle titleText{};
  TextStyle itemText{};
  StyleSet panelStyles{};
  StyleSet itemStyles{};
  Insets padding{10, 10, 10, 10};
  int16_t rowHeight = 40;
  int16_t gap = 2;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  bool dimBackground = true;
};

template <size_t MaxInteractions>
void contextMenu(Frame<MaxInteractions>& frame, Rect rect, const ContextMenuProps& props) {
  if (props.dimBackground) frame.target().fill(frame.screen(), Paint::dither(Color::LightGray));
  StyleSet panelStyles = props.panelStyles.unset() ? defaultPopupStyles() : props.panelStyles;
  const BoxStyle& panel = panelStyles.resolve(StateNormal);
  frame.target().fill(rect, panel.background, panel.radius, panel.corners);
  if (panel.border.kind != PaintKind::None && panel.borderWidth > 0) {
    frame.target().stroke(rect, panel.border, panel.borderWidth, panel.radius, panel.corners);
  }

  Rect content = rect.inset(props.padding);
  if (props.title) {
    const int16_t titleH = frame.target().lineHeight(props.titleText.font);
    frame.target().text(Rect{content.x, content.y, content.width, titleH}, props.title, props.titleText);
    content.y = static_cast<int16_t>(content.y + titleH + props.gap);
    content.height = static_cast<int16_t>(content.height - titleH - props.gap);
  }

  for (uint8_t i = 0; props.options && i < props.optionCount; ++i) {
    Rect row{content.x, static_cast<int16_t>(content.y + i * (props.rowHeight + props.gap)), content.width,
             props.rowHeight};
    if (row.bottom() > content.bottom()) break;
    ButtonProps bp;
    bp.label = props.options[i].label;
    bp.action = props.options[i].action;
    bp.value = props.options[i].value;
    bp.inputMask = props.inputMask;
    bp.state = props.options[i].state;
    bp.enabled = props.options[i].enabled;
    bp.text = props.itemText;
    bp.styles = props.itemStyles;
    bp.minTouchSize = frame.device().minTouchSize;
    button(frame, row, bp);
  }
}

}  // namespace ui
}  // namespace freeink
