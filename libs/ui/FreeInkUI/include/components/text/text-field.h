#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct TextFieldProps {
  const char* text = nullptr;
  TextStyle textStyle{};
  StyleSet styles{};
  uint16_t cursor = 0;
  bool cursorVisible = false;
  bool cursorBlock = false;
  bool selected = false;
  bool disabled = false;
  int16_t horizontalPadding = 6;
};

template <size_t MaxInteractions>
void textField(Frame<MaxInteractions>& frame, Rect rect, const TextFieldProps& props) {
  State state = props.disabled ? StateDisabled : (props.selected ? StateSelected : StateNormal);
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  const BoxStyle& style = styles.resolve(state);
  frame.target().fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    frame.target().stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }
  Rect content = rect.inset(Insets{0, props.horizontalPadding, 0, props.horizontalPadding});
  frame.target().text(content, props.text, props.textStyle);
  if (props.cursorVisible) {
    const int16_t lh = frame.target().lineHeight(props.textStyle.font);
    const int16_t cy = static_cast<int16_t>(content.y + (content.height - lh) / 2);
    int16_t cx = content.x;
    if (props.text && props.cursor > 0) {
      // Measure the prefix before the cursor in chunks so long inputs (URLs,
      // passphrases) place the cursor correctly without a large stack buffer.
      char buf[64];
      uint16_t consumed = 0;
      while (props.text[consumed] != '\0' && consumed < props.cursor) {
        uint16_t len = 0;
        while (props.text[consumed + len] != '\0' && consumed + len < props.cursor && len < sizeof(buf) - 1) {
          buf[len] = props.text[consumed + len];
          ++len;
        }
        // Never split a UTF-8 sequence across chunks: if the next byte is a
        // continuation byte, back off to the last sequence start.
        while (len > 1 && (props.text[consumed + len] & 0xC0) == 0x80 && (buf[len - 1] & 0x80) != 0) {
          --len;
        }
        if (len == 0) break;
        buf[len] = '\0';
        cx = static_cast<int16_t>(cx + frame.target().measureText(props.textStyle.font, buf, props.textStyle).width);
        consumed = static_cast<uint16_t>(consumed + len);
      }
    }
    if (props.cursorBlock) {
      frame.target().fill(Rect{cx, cy, 8, lh}, Paint::solid(Color::Black));
    } else {
      frame.target().fill(Rect{cx, cy, 2, lh}, Paint::solid(Color::Black));
    }
  }
}

}  // namespace ui
}  // namespace freeink
