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
  // Switches to the next enabled script layout (Latin <-> Cyrillic <-> ...).
  // Distinct from Mode, which pages between letters and symbols within one
  // script. The label is always supplied by the app through KeyboardProps,
  // since which layout is active is app state the static tables cannot know.
  Lang,
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
  int16_t gap = 6;
  int16_t minTouchSize = 28;
  uint8_t radius = 0;
  bool inactiveSelection = false;
};

// Geometry shared by number pads with a readout and a separate delete button.
// A wide body can place the readout beside the grid, reclaiming its full row
// height for the keys without changing their familiar order or touch routing.
// readoutWidth is the measured text width; the value and delete button share
// a centered row in the side column.
struct KeyGridReadoutLayout {
  Rect readout;
  Rect backspace;
  Rect grid;
};

inline KeyGridReadoutLayout keyGridReadoutLayout(Rect body, int16_t readoutHeight, int16_t readoutWidth, int16_t gap,
                                                 int16_t padding, bool beside) {
  KeyGridReadoutLayout layout;
  const int16_t groupWidth = static_cast<int16_t>(readoutWidth + gap + readoutHeight);
  const int16_t sideWidth = body.width / 3 > groupWidth ? static_cast<int16_t>(body.width / 3) : groupWidth;
  const int16_t gridWidth = static_cast<int16_t>(body.width - sideWidth - gap - padding);
  if (beside && body.width > body.height && gridWidth >= readoutHeight * 3 + gap * 2 &&
      body.height >= readoutHeight + padding) {
    const int16_t top = static_cast<int16_t>(body.y + (body.height - padding - readoutHeight) / 2);
    const int16_t left = static_cast<int16_t>(body.x + (sideWidth - groupWidth) / 2);
    layout.readout = Rect{left, top, readoutWidth, readoutHeight};
    layout.backspace = Rect{static_cast<int16_t>(layout.readout.right() + gap), top, readoutHeight, readoutHeight};
    layout.grid = Rect{static_cast<int16_t>(body.x + sideWidth + gap), body.y, gridWidth,
                       static_cast<int16_t>(body.height - padding)};
  } else {
    layout.readout = Rect{body.x, body.y, body.width, readoutHeight};
    layout.backspace = Rect{static_cast<int16_t>(body.right() - readoutHeight), body.y, readoutHeight, readoutHeight};
    layout.grid =
        Rect{static_cast<int16_t>(body.x + padding), static_cast<int16_t>(body.y + readoutHeight + gap),
             static_cast<int16_t>(body.width - padding * 2), clampI16(body.height - readoutHeight - gap - padding)};
  }
  return layout;
}

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
        bp.icon = lucideDeleteIcon28();
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
