#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/button.h"

namespace freeink {
namespace ui {

struct DialogOption {
  const char* label = nullptr;
  ActionId action = NO_ACTION;
  int16_t value = 0;
  State state = StateNormal;
  bool enabled = true;
  // Horizontal rows only: fixed slot width (e.g. a square "+" button); 0
  // shares the remaining width equally with the other flexible options.
  int16_t width = 0;
};

struct OptionDialogProps {
  // Three optional text slots, top to bottom: a small caption ("Skip this
  // event?"), a prominent multi-line headline (an event or book title), and a
  // body line (a time range, a detail). Each renders with its own style and
  // honors that style's alignment and maxLines.
  const char* title = nullptr;
  const char* headline = nullptr;
  const char* message = nullptr;
  const DialogOption* options = nullptr;
  uint8_t optionCount = 0;
  // Reserved band between the text slots and the buttons for caller-drawn
  // content (a slider, a preview). optionDialog() returns its rect.
  int16_t contentHeight = 0;
  TextStyle titleText{};
  TextStyle headlineText{};
  TextStyle messageText{};
  TextStyle buttonText{};
  StyleSet styles{};        // dialog panel
  StyleSet buttonStyles{};  // option buttons
  Insets padding{12, 16, 12, 16};
  int16_t buttonHeight = 44;
  int16_t gap = 8;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  // Stack options vertically (one per row) instead of in one bottom row;
  // better for narrow portrait panels or more than three options.
  bool verticalOptions = false;
  // Dim everything behind the dialog with a dither so stale chrome reads as
  // inactive on e-paper.
  bool dimBackground = false;
};

// Panel height optionDialog needs at the given width — thin sugar over
// measureWrappedText so callers size the dialog instead of guessing:
//
//   const int16_t h = optionDialogHeight(ui.target(), props, 340);
//   optionDialog(ui, centeredRect(ui.safeRect(), {340, h}), props);
inline int16_t optionDialogHeight(const DrawTarget& target, const OptionDialogProps& props, const int16_t width) {
  const int16_t contentW = static_cast<int16_t>(width - props.padding.left - props.padding.right);
  int16_t height = static_cast<int16_t>(props.padding.top + props.padding.bottom);
  if (props.title) {
    // Honors titleText.maxLines (default 1), so long captions can opt in to
    // wrapping and the panel grows to match.
    height = static_cast<int16_t>(height + measureWrappedText(target, props.title, props.titleText, contentW).height +
                                  props.gap);
  }
  if (props.headline) {
    height = static_cast<int16_t>(height + measureWrappedText(target, props.headline, props.headlineText, contentW).height +
                                  props.gap);
  }
  if (props.message) {
    height = static_cast<int16_t>(height + measureWrappedText(target, props.message, props.messageText, contentW).height);
  }
  if (props.contentHeight > 0) {
    height = static_cast<int16_t>(height + props.gap + props.contentHeight);
  }
  if (props.options && props.optionCount > 0) {
    const int16_t buttonsH =
        props.verticalOptions
            ? static_cast<int16_t>(props.optionCount * props.buttonHeight + (props.optionCount - 1) * props.gap)
            : props.buttonHeight;
    height = static_cast<int16_t>(height + props.gap + buttonsH);
  }
  return height;
}

// Returns the reserved content band (empty when contentHeight is 0) so the
// caller can draw its own control into the dialog.
template <size_t MaxInteractions>
Rect optionDialog(Frame<MaxInteractions>& frame, Rect rect, const OptionDialogProps& props) {
  if (props.dimBackground) {
    frame.target().fill(frame.screen(), Paint::dither(Color::LightGray));
  }
  StyleSet styles = props.styles.unset() ? defaultPopupStyles() : props.styles;
  const BoxStyle& style = styles.resolve(StateNormal);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }

  Rect content = rect.inset(props.padding);
  int16_t cursorY = content.y;
  if (props.title) {
    // Same one-pass wrap-and-draw as the headline below, so the height
    // reserved by optionDialogHeight always matches the rendered lines.
    const int16_t lh = frame.target().lineHeight(props.titleText.font);
    TextStyle lineStyle = props.titleText;
    lineStyle.align = TextAlign::Left;  // runs arrive already positioned
    lineStyle.maxLines = 1;
    int16_t lines = 0;
    layoutText(frame.target(), Rect{content.x, cursorY, content.width, 1}, props.title, props.titleText,
               [&](const char* line, Rect r) {
                 frame.target().text(r, line, lineStyle);
                 ++lines;
               });
    cursorY = static_cast<int16_t>(cursorY + lines * lh + props.gap);
  }

  if (props.headline) {
    // Wrap and draw in one layoutText pass so the reserved height always
    // matches the rendered lines; the height-1 rect top-anchors the block at
    // the cursor (vertical centering only engages when the rect is taller
    // than the text).
    const int16_t lh = frame.target().lineHeight(props.headlineText.font);
    TextStyle lineStyle = props.headlineText;
    lineStyle.align = TextAlign::Left;  // runs arrive already positioned
    lineStyle.maxLines = 1;
    int16_t lines = 0;
    layoutText(frame.target(), Rect{content.x, cursorY, content.width, 1}, props.headline, props.headlineText,
               [&](const char* line, Rect r) {
                 frame.target().text(r, line, lineStyle);
                 ++lines;
               });
    cursorY = static_cast<int16_t>(cursorY + lines * lh + props.gap);
  }

  int16_t buttonsH = 0;
  if (props.options && props.optionCount > 0) {
    buttonsH = props.verticalOptions
                   ? static_cast<int16_t>(props.optionCount * props.buttonHeight +
                                          (props.optionCount - 1) * props.gap)
                   : props.buttonHeight;
  }

  // Caller-drawn content band, directly above the buttons.
  Rect contentBand{};
  int16_t slotsBottom = static_cast<int16_t>(content.bottom() - buttonsH - (buttonsH ? props.gap : 0));
  if (props.contentHeight > 0) {
    contentBand = Rect{content.x, static_cast<int16_t>(slotsBottom - props.contentHeight), content.width,
                       props.contentHeight};
    slotsBottom = static_cast<int16_t>(contentBand.y - props.gap);
  }

  if (props.message) {
    Rect messageRect{content.x, cursorY, content.width, static_cast<int16_t>(slotsBottom - cursorY)};
    drawText(frame.target(), messageRect, props.message, props.messageText);
  }

  if (!props.options || props.optionCount == 0) return contentBand;
  Rect buttons{content.x, static_cast<int16_t>(content.bottom() - buttonsH), content.width, buttonsH};
  // Horizontal rows: fixed-width options keep their width, the rest split the
  // remainder equally.
  int32_t fixedW = 0;
  uint8_t flexCount = 0;
  for (uint8_t i = 0; i < props.optionCount; ++i) {
    if (props.options[i].width > 0) fixedW += props.options[i].width;
    else ++flexCount;
  }
  const int32_t flexTotal = buttons.width - (props.optionCount - 1) * props.gap - fixedW;
  const int16_t flexW = flexCount > 0 && flexTotal > 0 ? static_cast<int16_t>(flexTotal / flexCount) : 0;
  int16_t optionX = buttons.x;
  for (uint8_t i = 0; i < props.optionCount; ++i) {
    const DialogOption& option = props.options[i];
    Rect optionRect;
    if (props.verticalOptions) {
      optionRect = Rect{buttons.x, static_cast<int16_t>(buttons.y + i * (props.buttonHeight + props.gap)),
                        buttons.width, props.buttonHeight};
    } else {
      const int16_t w = option.width > 0 ? option.width : flexW;
      optionRect = Rect{optionX, buttons.y, w, buttons.height};
      optionX = static_cast<int16_t>(optionX + w + props.gap);
    }
    ButtonProps bp;
    bp.label = option.label;
    bp.action = option.action;
    bp.value = option.value;
    bp.inputMask = props.inputMask;
    bp.state = option.state;
    bp.text = props.buttonText;
    bp.styles = props.buttonStyles;
    bp.minTouchSize = frame.device().minTouchSize;
    bp.enabled = option.enabled;
    button(frame, optionRect, bp);
  }
  return contentBand;
}

}  // namespace ui
}  // namespace freeink
