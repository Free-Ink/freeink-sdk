#pragma once

// Optional adapter: builds a freeink::ui::InputSnapshot from the SDK's
// InputManager each frame. Include this header from application code only —
// FreeInkUI itself stays dependency-free, and PlatformIO will only require
// InputManager when a compiled source actually includes this file.
//
// Touch coordinates pass through in InputManager's mapped panel space. If the
// UI renders in a rotated frame, remap snapshot.touchX/touchY before handing
// the snapshot to Frame.

#include <FreeInkUI.h>
#include <InputManager.h>

namespace freeink {
namespace ui {

// Which physical button drives which semantic input. Defaults follow the
// SDK-wide convention: UP/DOWN move focus, LEFT/RIGHT page, CONFIRM/BACK act.
struct ButtonBindings {
  uint8_t focusPrev = InputManager::BTN_UP;
  uint8_t focusNext = InputManager::BTN_DOWN;
  uint8_t confirm = InputManager::BTN_CONFIRM;
  uint8_t back = InputManager::BTN_BACK;
  uint8_t prev = InputManager::BTN_LEFT;
  uint8_t next = InputManager::BTN_RIGHT;
};

// Call after InputManager::update(). Long-press and swipe synthesis stay
// app-owned (InputManager exposes no per-touch hold/movement history); set
// snapshot.longPress / swipeLeft / swipeRight afterwards if the app tracks
// gestures itself.
inline InputSnapshot snapshotFrom(const InputManager& input, const ButtonBindings& bindings = ButtonBindings{}) {
  InputSnapshot snapshot;
  snapshot.focusPrev = input.wasPressed(bindings.focusPrev);
  snapshot.focusNext = input.wasPressed(bindings.focusNext);
  snapshot.confirm = input.wasPressed(bindings.confirm);
  snapshot.back = input.wasPressed(bindings.back);
  snapshot.prev = input.wasPressed(bindings.prev);
  snapshot.next = input.wasPressed(bindings.next);

  if (input.hasTouch()) {
    snapshot.touchPressed = input.wasTouchPressed();
    snapshot.touchReleased = input.wasTouchReleased();
    const InputManager::TouchPoint point = input.getTouchPoint();
    if (point.valid) {
      snapshot.touchX = static_cast<int16_t>(point.x);
      snapshot.touchY = static_cast<int16_t>(point.y);
      snapshot.touchHeld = !snapshot.touchReleased;
    }
  }
  return snapshot;
}

// Orientation-aware variant: taps arrive as InputManager's normalized
// panel-native coordinates and land in the snapshot already mapped to the
// device's logical frame via touchToLogical(). flipX/flipY compensate for
// mirrored panel mounting (a board property — set once per device, verified
// on the bench, not rediscovered per app).
inline InputSnapshot snapshotFrom(const InputManager& input, const DeviceContext& device,
                                  const bool touchFlipX = false, const bool touchFlipY = false,
                                  const ButtonBindings& bindings = ButtonBindings{}) {
  InputSnapshot snapshot = snapshotFrom(input, bindings);
  snapshot.touchPressed = false;
  snapshot.touchReleased = false;
  snapshot.touchHeld = false;
  float nx = 0.0f;
  float ny = 0.0f;
  // Live contact position for InputDrag interactions (sliders): mapped like
  // taps, delivered every frame while the finger is down.
  if (input.hasTouch() && input.isTouchHeldAt(nx, ny)) {
    const Point p = touchToLogical(device, nx, ny, touchFlipX, touchFlipY);
    snapshot.touchHeld = true;
    snapshot.touchX = p.x;
    snapshot.touchY = p.y;
  }
  // Press edge, mapped like the tap: lets interaction routing mark the element
  // under the finger active on touch-down (pressed-style feedback) before the
  // release delivers the action.
  if (input.hasTouch() && input.wasTouchPressedAt(nx, ny)) {
    const Point p = touchToLogical(device, nx, ny, touchFlipX, touchFlipY);
    snapshot.touchPressed = true;
    snapshot.touchX = p.x;
    snapshot.touchY = p.y;
  }
  if (input.hasTouch() && input.wasTouchTap(nx, ny)) {
    const Point p = touchToLogical(device, nx, ny, touchFlipX, touchFlipY);
    snapshot.touchReleased = true;
    snapshot.touchX = p.x;
    snapshot.touchY = p.y;
  }
  // Raw release the tap classifier didn't report (swipe end, drag-off).
  // Deliver it off-target: routing dispatches nothing, but the interaction
  // buffer drops its pressed-element state — otherwise that state survives
  // the next frame's rebuild and paints a phantom active highlight on
  // whatever lands in the same slot (e.g. after a swipe pages a list).
  if (input.hasTouch() && !snapshot.touchReleased && input.wasTouchReleased()) {
    snapshot.touchReleased = true;
    snapshot.touchX = -1;
    snapshot.touchY = -1;
  }
  return snapshot;
}

}  // namespace ui
}  // namespace freeink
