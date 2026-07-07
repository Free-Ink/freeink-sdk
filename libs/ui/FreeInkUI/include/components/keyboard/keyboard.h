#pragma once

#include "../../FreeInkUICore.h"
#include "../controls/button.h"
#include "../keyboard/key-grid.h"

namespace freeink {
namespace ui {

constexpr int16_t QWERTY_KEY_SHIFT = -1;
constexpr int16_t QWERTY_KEY_MODE = -2;
constexpr int16_t QWERTY_KEY_BACKSPACE = 8;
constexpr int16_t QWERTY_KEY_ENTER = 13;
constexpr int16_t QWERTY_KEY_SPACE = 32;

enum class KeyboardLayoutId : uint8_t {
  QwertyEn,
  AzertyFr,
  QwertzDe,
  SpanishEs,
};

struct KeyboardKey {
  const char* label = nullptr;   // UTF-8 visual label.
  const char* output = nullptr;  // UTF-8 text the app may insert for normal keys.
  KeyKind kind = KeyKind::Normal;
  State state = StateNormal;
  int16_t value = 0;       // Stable key id returned in ActionEvent::value.
  uint8_t widthUnits = 1;  // Relative visual width within the row.
  bool enabled = true;
};

struct KeyboardRow {
  const KeyboardKey* keys = nullptr;
  uint8_t count = 0;
  uint8_t insetUnits = 0;
};

struct KeyboardLayout {
  const KeyboardRow* rows = nullptr;
  uint8_t rowCount = 0;
};

struct KeyboardProps {
  const KeyboardLayout* layout = nullptr;
  ActionId keyAction = NO_ACTION;
  ActionId shiftAction = NO_ACTION;
  ActionId modeAction = NO_ACTION;
  ActionId deleteAction = NO_ACTION;
  ActionId okAction = NO_ACTION;
  uint16_t inputMask = InputDefault;
  int16_t selectedIndex = -1;
  TextStyle labelText{};
  StyleSet keyStyles{};
  Insets padding{5, 5, 5, 5};
  int16_t gap = 3;
  int16_t minTouchSize = 28;
  uint8_t keyRadius = 0;
  bool inactiveSelection = false;
};

const KeyboardLayout& builtinKeyboardLayout(KeyboardLayoutId id, bool shifted = false, bool symbols = false);

// The UTF-8 text a key id inserts under the given layout (nullptr for
// shift/mode/delete/OK and unknown ids). Keys report stable ids in
// ActionEvent::value — ASCII keys their code point, localized keys (é, ñ, ß)
// ids above 1000 — so casting the value to char corrupts non-ASCII layouts;
// always insert through this lookup.
const char* keyboardOutputFor(const KeyboardLayout& layout, int16_t value);

// The editing state every on-screen-keyboard consumer otherwise hand-rolls:
// the shift/symbols layers, layout-correct UTF-8 append, and multi-byte-aware
// backspace over a caller-owned buffer. Bind a buffer with attach(), route the
// keyboard's key/shift/mode/delete actions to the matching methods, and mirror
// the flags into the keyboard props each frame (QwertyKeyboardProps takes
// layout/shifted/symbols verbatim; see applyEntry() in qwerty-keyboard.h).
class KeyboardEntry {
 public:
  KeyboardLayoutId layout = KeyboardLayoutId::QwertyEn;
  bool shifted = false;
  bool symbols = false;

  // Point the entry at a NUL-terminated buffer (capacity includes the NUL).
  // Editing resumes from the buffer's current contents.
  void attach(char* buffer, size_t capacity, bool startShifted = false) {
    buf_ = buffer;
    cap_ = buffer ? capacity : 0;
    len_ = 0;
    if (buf_) {
      while (len_ + 1 < cap_ && buf_[len_]) ++len_;
      buf_[len_] = 0;
    }
    shifted = startShifted;
    symbols = false;
  }

  // Insert the key's layout output (ActionEvent::value from the key action).
  // Shift auto-releases after one letter; symbol pages are sticky.
  bool key(int16_t value) {
    const char* out = keyboardOutputFor(builtinKeyboardLayout(layout, shifted, symbols), value);
    if (!symbols) shifted = false;
    if (!out || !buf_) return false;
    size_t n = 0;
    while (out[n]) ++n;
    if (len_ + n + 1 > cap_) return false;
    for (size_t i = 0; i < n; ++i) buf_[len_ + i] = out[i];
    len_ += n;
    buf_[len_] = 0;
    return true;
  }

  // Remove the last character (one code point, not one byte).
  bool backspace() {
    if (!buf_ || len_ == 0) return false;
    --len_;
    while (len_ > 0 && (static_cast<uint8_t>(buf_[len_]) & 0xC0) == 0x80) --len_;
    buf_[len_] = 0;
    return true;
  }

  // The shift slot: letter case in the letter layers, page one/two in symbols.
  void shift() { shifted = !shifted; }

  // The mode slot ("?123"/"ABC"): enter or leave the symbol layers.
  void mode() {
    symbols = !symbols;
    shifted = false;
  }

  void clear(bool startShifted = false) {
    if (buf_ && cap_) buf_[0] = 0;
    len_ = 0;
    shifted = startShifted;
    symbols = false;
  }

  const char* text() const { return buf_ ? buf_ : ""; }
  size_t length() const { return len_; }
  bool empty() const { return len_ == 0; }

 private:
  char* buf_ = nullptr;
  size_t cap_ = 0;
  size_t len_ = 0;
};

template <size_t MaxInteractions>
void keyboard(Frame<MaxInteractions>& frame, Rect rect, const KeyboardProps& props) {
  if (!props.layout || !props.layout->rows || props.layout->rowCount == 0) return;
  StyleSet styles = props.keyStyles.unset() ? defaultButtonStyles() : props.keyStyles;
  if (props.keyRadius > 0) setStyleRadius(styles, props.keyRadius);
  TextStyle keyText = props.labelText;
  keyText.align = TextAlign::Center;
  keyText.maxLines = 1;
  rect = rect.inset(props.padding);
  if (rect.empty() || rect.width < 10 || rect.height < 10) return;
  const int16_t gap = props.gap < 0 ? 0 : props.gap;
  const int16_t rowH = static_cast<int16_t>((rect.height - gap * (props.layout->rowCount - 1)) / props.layout->rowCount);
  int16_t logicalIndex = 0;

  auto actionFor = [&](KeyKind kind) {
    if (kind == KeyKind::Shift && props.shiftAction != NO_ACTION) return props.shiftAction;
    if (kind == KeyKind::Mode && props.modeAction != NO_ACTION) return props.modeAction;
    if (kind == KeyKind::Delete && props.deleteAction != NO_ACTION) return props.deleteAction;
    if (kind == KeyKind::Ok && props.okAction != NO_ACTION) return props.okAction;
    return props.keyAction;
  };

  auto drawKey = [&](Rect keyRect, const KeyboardKey& key, int16_t selectedIndex) {
    State state = StateNormal;
    if (props.selectedIndex == selectedIndex) state |= props.inactiveSelection ? StateFocused : StateSelected;
    if (!key.enabled || key.kind == KeyKind::Disabled) state |= StateDisabled;
    const ActionId action = actionFor(key.kind);
    ButtonProps bp;
    bp.label = (key.kind == KeyKind::Space || key.kind == KeyKind::Delete) ? nullptr : key.label;
    bp.action = action;
    bp.value = key.value;
    bp.inputMask = props.inputMask;
    bp.state = state;
    bp.text = keyText;
    bp.styles = styles;
    bp.minTouchSize = props.minTouchSize;
    bp.radius = props.keyRadius;
    bp.enabled = key.enabled && key.kind != KeyKind::Disabled;
    button(frame, keyRect, bp);

    if (key.kind == KeyKind::Delete) {
      // Size the delete glyph from the label font so it reads at the same
      // weight as neighboring key labels; the 16px source art carries ~3px of
      // internal margin, so the box runs slightly over the line height.
      const Paint ink = styles.resolve(frame.stateFor(action, key.value, state)).foreground;
      const int16_t lh = frame.target().lineHeight(keyText.font);
      int16_t iconSize = static_cast<int16_t>(lh + lh / 8);
      if (iconSize < 16) iconSize = 16;
      const int16_t maxSize = keyRect.height < keyRect.width ? keyRect.height : keyRect.width;
      if (iconSize > maxSize) iconSize = maxSize;
      frame.target().bitmap(centeredRect(keyRect, Size{iconSize, iconSize}), lucideDeleteIcon16(),
                            BitmapMode::Contain, ink);
      return;
    }

    if (key.kind != KeyKind::Space) return;
    const Paint ink = styles.resolve(frame.stateFor(action, key.value, state)).foreground;
    const int16_t cx = static_cast<int16_t>(keyRect.x + keyRect.width / 2);
    const int16_t cy = static_cast<int16_t>(keyRect.y + keyRect.height / 2);
    const int16_t half = static_cast<int16_t>(keyRect.width * 3 / 10);
    frame.target().line(Point{static_cast<int16_t>(cx - half), static_cast<int16_t>(cy + 3)},
                        Point{static_cast<int16_t>(cx + half), static_cast<int16_t>(cy + 3)}, 3, ink);
  };

  for (uint8_t row = 0; row < props.layout->rowCount; ++row) {
    const KeyboardRow& layoutRow = props.layout->rows[row];
    if (!layoutRow.keys || layoutRow.count == 0) continue;
    uint16_t units = static_cast<uint16_t>(layoutRow.insetUnits * 2);
    for (uint8_t col = 0; col < layoutRow.count; ++col) {
      units = static_cast<uint16_t>(units + (layoutRow.keys[col].widthUnits ? layoutRow.keys[col].widthUnits : 1));
    }
    const int16_t unitW = static_cast<int16_t>((rect.width - gap * (layoutRow.count - 1)) / units);
    const int16_t y = static_cast<int16_t>(rect.y + row * (rowH + gap));
    int16_t x = static_cast<int16_t>(rect.x + layoutRow.insetUnits * unitW);
    const int16_t rowRight = static_cast<int16_t>(rect.right() - layoutRow.insetUnits * unitW);
    for (uint8_t col = 0; col < layoutRow.count; ++col) {
      const KeyboardKey& key = layoutRow.keys[col];
      const uint8_t keyUnits = key.widthUnits ? key.widthUnits : 1;
      const int16_t w = col == layoutRow.count - 1 ? static_cast<int16_t>(rowRight - x)
                                                   : static_cast<int16_t>(unitW * keyUnits);
      drawKey(Rect{x, y, w, rowH}, key, logicalIndex++);
      x = static_cast<int16_t>(x + w + gap);
    }
  }
}

}  // namespace ui
}  // namespace freeink
