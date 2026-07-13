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
  // UTF-8 alternate output for normal keys: drawn as a small corner hint and
  // inserted on long-press (ActionEvent::longPress; see keyboardAltOutputFor).
  const char* alt = nullptr;
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
  // Style for the small corner hint drawn on keys that define `alt`. The
  // color follows the key's resolved foreground; set the font here (typically
  // the small slot).
  TextStyle altText{};
  StyleSet keyStyles{};
  Insets padding{5, 5, 5, 5};
  int16_t gap = 3;
  int16_t minTouchSize = 28;
  uint8_t keyRadius = 0;
  // Extra hit area below the last row's keys. Fingers occlude the key being
  // pressed and land low, and the last row has no key beneath it to catch the
  // miss — extend its hit band down to whatever sits below (hints bar, screen
  // edge). Visual rects are unchanged.
  int16_t bottomHitOverflow = 0;
  bool inactiveSelection = false;
};

// `numberRow` prepends a dedicated digit row (with shift-symbol alternates on
// long-press) to the letter layers — for entry fields where digits must stay
// one tap away (passwords, hosts, ports). The symbol layers already carry
// digits, so they ignore the flag. Shift swaps the row to symbols-primary on
// QwertyEn; the other languages keep their single letter layer.
const KeyboardLayout& builtinKeyboardLayout(KeyboardLayoutId id, bool shifted = false, bool symbols = false,
                                            bool numberRow = false);

// The UTF-8 text a key id inserts under the given layout (nullptr for
// shift/mode/delete/OK and unknown ids). Keys report stable ids in
// ActionEvent::value — ASCII keys their code point, localized keys (é, ñ, ß)
// ids above 1000 — so casting the value to char corrupts non-ASCII layouts;
// always insert through this lookup.
const char* keyboardOutputFor(const KeyboardLayout& layout, int16_t value);

// The UTF-8 alternate a key id inserts on long-press. Explicit KeyboardKey::alt
// wins; ASCII letter keys without one fall back to the opposite case (the
// phone-keyboard hold-for-capital convention). Returns nullptr when the key
// has no alternate. The case-flip result lives in a static buffer — call from
// one task (the UI loop), and consume before the next call.
const char* keyboardAltOutputFor(const KeyboardLayout& layout, int16_t value);

// Touch hold routing for e-paper keyboards: fires the long-press at the
// threshold while the finger is still down (waiting for the release feels
// broken next to a 1s panel refresh) and swallows the eventual release.
//
// Feed one update() per loop pass from the app's level-triggered touch state:
//   pressedDown  — a tap-candidate contact is currently on the screen
//                  (true EVERY pass while held, not just the first)
//   tapped       — a tap release was reported this pass
//   inContact    — any contact is present (ungated; catches swipe drift)
// The router latches transitions itself — feeding level-triggered signals is
// the point, since getting that latch wrong (restarting the hold timer every
// pass) is the natural bug every hand-rolled version hits.
//
// Returns the dispatched ActionEvent (tap or threshold long-press) plus an
// activeChanged flag for touch-down highlight repaints.
class TouchHoldRouter {
 public:
  uint16_t holdMs = 350;
  // Keys with this value use the longer threshold (delete's clear-all hold).
  uint16_t overrideHoldMs = 900;
  int16_t overrideValue = QWERTY_KEY_BACKSPACE;

  struct Result {
    ActionEvent event{};
    bool activeChanged = false;

    explicit operator bool() const { return static_cast<bool>(event); }
  };

  template <size_t MaxInteractions>
  Result update(InteractionBuffer<MaxInteractions>& interactions, const bool pressedDown, const int16_t px,
                const int16_t py, const bool tapped, const int16_t tx, const int16_t ty, const bool inContact,
                const uint32_t nowMs) {
    Result result;

    // Release first: if a tap report ever coincides with a press frame, the
    // press path returning early must not eat the release.
    if (tapped) {
      const bool swallow = longFired_;  // the long-press already dispatched at threshold
      const int16_t heldActive = interactions.activeIndex();
      held_ = false;
      longFired_ = false;
      if (!swallow) {
        InputSnapshot release{};
        release.touchReleased = true;
        release.touchX = tx;
        release.touchY = ty;
        result.event = interactions.route(release);
        // Tap slop means the release can land off the pressed key (fingers
        // occlude and slide low on e-paper). A tap is bounded by the slop
        // radius, so dispatching the key the press landed on is what the
        // user meant — a >slop drag is a swipe and never reaches here.
        if (!result.event && heldActive >= 0) {
          const Interaction& held = interactions.data()[heldActive];
          if (!hasState(held.state, StateDisabled) && acceptsInput(held.inputMask, InputTouch)) {
            result.event = ActionEvent{held.action, held.value, held.state};
          }
        }
      }
      return result;
    }

    if (pressedDown) {
      // A long-press already dispatched for this contact: hold everything
      // (including the timer — the synthesized release cleared the active
      // index, which must not read as a key change) until the finger lifts.
      if (longFired_) return result;

      const int16_t prevActive = interactions.activeIndex();
      InputSnapshot press{};
      press.touchPressed = true;
      press.touchX = px;
      press.touchY = py;
      interactions.route(press);
      const int16_t active = interactions.activeIndex();

      // New contact, or the finger slid onto a different key: restart the
      // hold timer and repaint the highlight.
      if (!held_ || active != prevActive) {
        held_ = true;
        startMs_ = nowMs;
        result.activeChanged = active != prevActive;
      }

      if (active >= 0) {
        const uint32_t threshold = interactions.data()[active].value == overrideValue ? overrideHoldMs : holdMs;
        if (nowMs - startMs_ >= threshold) {
          longFired_ = true;
          InputSnapshot release{};
          release.touchReleased = true;
          release.longPress = true;
          release.touchX = px;
          release.touchY = py;
          result.event = interactions.route(release);
        }
      }
      return result;
    }

    if (held_ && !inContact) {
      // Contact ended without a tap (drifted into a swipe): reset and clear
      // the active highlight. The empty release lands at (0,0); interaction
      // tables that place a target there would mis-dispatch, so keep the
      // origin corner target-free when using this router.
      held_ = false;
      longFired_ = false;
      InputSnapshot cancel{};
      cancel.touchReleased = true;
      interactions.route(cancel);
      result.activeChanged = true;
    }
    return result;
  }

  void reset() {
    held_ = false;
    longFired_ = false;
    startMs_ = 0;
  }

 private:
  bool held_ = false;
  bool longFired_ = false;
  uint32_t startMs_ = 0;
};

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
  bool numberRow = false;

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
  // Shift auto-releases after one letter; symbol pages are sticky. Pass
  // ActionEvent::longPress to insert the key's alternate output when it has
  // one (falls back to the normal output otherwise).
  bool key(int16_t value, bool longPress = false) {
    const KeyboardLayout& current = builtinKeyboardLayout(layout, shifted, symbols, numberRow);
    const char* out = longPress ? keyboardAltOutputFor(current, value) : nullptr;
    if (!out) out = keyboardOutputFor(current, value);
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

  int16_t rowHitOverflow = 0;  // set per row: bottomHitOverflow on the last row
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
    bp.hitPadding.bottom = rowHitOverflow;
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

    if (key.kind == KeyKind::Normal && key.alt) {
      // Corner hint for the long-press alternate. Ink follows the key's
      // resolved foreground so the hint stays legible on selected/active keys.
      TextStyle altStyle = props.altText;
      altStyle.align = TextAlign::Right;
      altStyle.maxLines = 1;
      altStyle.color = styles.resolve(frame.stateFor(action, key.value, state)).foreground.color;
      const int16_t altLh = frame.target().lineHeight(altStyle.font);
      frame.target().text(Rect{static_cast<int16_t>(keyRect.x + 2), static_cast<int16_t>(keyRect.y + 2),
                               static_cast<int16_t>(keyRect.width - 3), altLh},
                          key.alt, altStyle);
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
    rowHitOverflow = row == props.layout->rowCount - 1 ? props.bottomHitOverflow : 0;
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
