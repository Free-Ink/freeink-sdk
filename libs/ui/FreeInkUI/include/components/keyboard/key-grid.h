#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/button.h"

namespace freeink {
namespace ui {

enum class KeyKind : uint8_t {
  Normal,
  Shift,
  Mode,
  Space,
  Delete,
  Ok,
  Disabled,
};

struct KeyGridKey {
  const char* label = nullptr;
  const char* secondaryLabel = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  KeyKind kind = KeyKind::Normal;
  State state = StateNormal;
  int16_t value = 0;
  bool enabled = true;
};

struct KeyGridProps {
  const KeyGridKey* keys = nullptr;
  uint8_t rows = 0;
  uint8_t cols = 0;
  int16_t selectedIndex = -1;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault;
  TextStyle labelText{};
  TextStyle secondaryText{};
  StyleSet keyStyles{};
  int16_t gap = 0;
  int16_t minTouchSize = 28;
  uint8_t radius = 0;
  bool inactiveSelection = false;
};

template <size_t MaxInteractions>
void keyGrid(Frame<MaxInteractions>& frame, Rect rect, const KeyGridProps& props) {
  if (!props.keys || props.rows == 0 || props.cols == 0) return;
  const int16_t cellW = static_cast<int16_t>((rect.width - (props.cols - 1) * props.gap) / props.cols);
  const int16_t cellH = static_cast<int16_t>((rect.height - (props.rows - 1) * props.gap) / props.rows);
  for (uint8_t row = 0; row < props.rows; ++row) {
    for (uint8_t col = 0; col < props.cols; ++col) {
      const uint8_t idx = static_cast<uint8_t>(row * props.cols + col);
      const KeyGridKey& key = props.keys[idx];
      Rect keyRect{static_cast<int16_t>(rect.x + col * (cellW + props.gap)),
                   static_cast<int16_t>(rect.y + row * (cellH + props.gap)), cellW, cellH};
      State state = key.state;
      if (props.selectedIndex == idx) state |= props.inactiveSelection ? StateFocused : StateSelected;
      if (!key.enabled || key.kind == KeyKind::Disabled) state |= StateDisabled;
      ButtonProps bp;
      bp.label = key.label;
      bp.icon = key.icon ? key.icon : resolveBitmap(frame.assets(), key.iconAsset);
      bp.action = props.action;
      bp.value = key.value;
      bp.inputMask = props.inputMask;
      bp.state = state;
      bp.text = props.labelText;
      bp.styles = props.keyStyles.unset() ? defaultKeyStyles() : props.keyStyles;
      bp.minTouchSize = props.minTouchSize;
      bp.radius = props.radius;
      bp.enabled = key.enabled && key.kind != KeyKind::Disabled;
      if (key.kind == KeyKind::Delete && !bp.icon) {
        bp.icon = lucideDeleteIcon16();
        bp.label = nullptr;
      }
      // Space renders glyph art instead of a text label.
      const bool glyphKey = key.kind == KeyKind::Space && !bp.icon;
      if (glyphKey) bp.label = nullptr;
      button(frame, keyRect, bp);
      if (glyphKey) {
        const Paint ink = bp.styles.resolve(frame.stateFor(props.action, key.value, state)).foreground;
        const int16_t cx = static_cast<int16_t>(keyRect.x + keyRect.width / 2);
        const int16_t cy = static_cast<int16_t>(keyRect.y + keyRect.height / 2);
        const int16_t half = static_cast<int16_t>(keyRect.width * 3 / 10);
        frame.target().line(Point{static_cast<int16_t>(cx - half), static_cast<int16_t>(cy + 3)},
                            Point{static_cast<int16_t>(cx + half), static_cast<int16_t>(cy + 3)}, 3, ink);
      }
      if (key.secondaryLabel && key.enabled) {
        Rect sec{static_cast<int16_t>(keyRect.right() - cellW / 3), keyRect.y, static_cast<int16_t>(cellW / 3),
                 static_cast<int16_t>(cellH / 2)};
        frame.target().text(sec, key.secondaryLabel, props.secondaryText);
      }
    }
  }
}

}  // namespace ui
}  // namespace freeink
