#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {
namespace media_detail {
inline bool present(const char* text) { return text && *text; }
template <typename Button>
inline bool hasButton(const Button& props) {
  return present(props.label) || props.icon || props.iconAsset;
}
inline int16_t min(int16_t a, int16_t b) { return a < b ? a : b; }
inline int16_t nonnegative(int16_t value) { return value > 0 ? value : 0; }
inline void box(DrawTarget& target, Rect rect, const BoxStyle& style, uint8_t edges) {
  target.fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0)
    drawBorderEdges(target, rect, style.border, style.borderWidth, style.radius, style.corners, edges);
}
template <size_t N, typename Props>
BoxStyle surface(Frame<N>& frame, Rect rect, const Props& props, int16_t value) {
  State state = props.enabled ? props.state : static_cast<State>(props.state | StateDisabled);
  if (props.enabled && !hasState(state, StateDisabled) && props.action != NO_ACTION)
    frame.hit(ensureMinTouchRect(rect, props.minTouchSize, frame.screen()),
              props.action, value, props.inputMask, state);
  StyleSet styles = props.styles.unset() ? defaultListRowStyles() : props.styles;
  if (props.radius > 0) setStyleRadius(styles, props.radius);
  BoxStyle style = styles.resolve(frame.stateFor(props.action, value, state));
  box(frame.target(), rect, style, props.borderEdges);
  return style;
}

// Draw only complete lines. Return the unused area for the next text block.
inline void text(DrawTarget& target, Rect& area, const char* label,
                 TextStyle style, uint8_t lines = 1, int16_t gap = 6) {
  gap = nonnegative(gap);
  const int16_t line = target.lineHeight(style.font);
  if (lines == 0 || !present(label) || area.empty() || line <= 0 || area.height < line) return;
  style.maxLines = static_cast<uint8_t>(min(lines, min(255, area.height / line)));
  const int16_t height = min(area.height, measureWrappedText(target, label, style, area.width).height);
  target.text(Rect{area.x, area.y, area.width, height}, label, style);
  const int16_t used = min(area.height, static_cast<int16_t>(height + gap));
  area.y += used;
  area.height -= used;
}
inline Rect take(Rect& area, int16_t height, int16_t gap = 12) {
  gap = nonnegative(gap);
  height = min(area.height, height);
  if (area.empty() || height <= 0) return {};
  Rect result{area.x, area.y, area.width, height};
  const int16_t used = min(area.height, static_cast<int16_t>(height + gap));
  area.y += used;
  area.height -= used;
  return result;
}
}  // namespace media_detail
}  // namespace ui
}  // namespace freeink
