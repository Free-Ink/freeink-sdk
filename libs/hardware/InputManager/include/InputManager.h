#pragma once

// FreeInk SDK — input abstraction.
//
// Reads buttons across board input styles (ADC resistor ladder, plain digital
// buttons, confirm-hold-for-back, five-key) selected from BoardConfig::ACTIVE,
// and exposes a uniform edge/level button API. Capacitive touch is abstracted
// behind the same object: hasTouch()/getTouchPoint() are inert on boards
// without a configured TouchController.

#include <Arduino.h>
#include <BoardConfig.h>
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstdint>

class InputManager {
 public:
  InputManager();
  void begin();
  uint8_t getState();

  // Call regularly from the main loop to update button and touch edge state.
  void update();

  // Level state from the last update().
  bool isPressed(uint8_t buttonIndex) const;

  // Current electrical level of the configured power-button GPIO, before
  // logical click/hold classification. False when the board has no such GPIO.
  bool isPowerButtonPhysicallyPressed() const;

  // Press edge since the previous update().
  bool wasPressed(uint8_t buttonIndex) const;

  // Any button press edge since the previous update().
  bool wasAnyPressed() const;

  // Release edge since the previous update().
  bool wasReleased(uint8_t buttonIndex) const;

  // Any button release edge since the previous update().
  bool wasAnyReleased() const;

  // True while a raw state change is still inside the debounce window (the last
  // raw sample differs from the committed state). A change only commits after
  // two consecutive matching samples, so hosts that poll slowly (e.g. a
  // sleep-sliced idle loop) should re-poll quickly while this is set —
  // otherwise a press shorter than the poll period lands in a single sample and
  // is dropped.
  bool isDebouncePending() const { return lastState != currentState; }

  // Duration between the first button press and final release.
  unsigned long getHeldTime() const;

  // Duration of the current or most recent power-button hold.
  unsigned long getPowerButtonHeldTime() const;

  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

  // Pins. POWER_BUTTON_PIN stays constexpr (consumers reference it in
  // pin-config contexts) and is bound to the build's default device; the input
  // code reads the runtime-active power pin internally so multi-device builds
  // stay correct.
  static constexpr int BUTTON_ADC_PIN_1 = 1;
  static constexpr int BUTTON_ADC_PIN_2 = 2;
  static constexpr int POWER_BUTTON_PIN = BoardConfig::DEFAULT_DEVICE.input.power;

  bool isPowerButtonPressed() const;

  static const char* getButtonName(uint8_t buttonIndex);

  // --- Capacitive touch (inert unless BoardConfig::ACTIVE.touch is configured)
  // ---
  struct TouchPoint {
    bool valid;
    uint16_t x;
    uint16_t y;
    unsigned long timestamp;
  };

  // Fixed-size contact snapshot for allocation-free multi-touch polling. `id`
  // is the GT911 track ID when `idsStable` is true. Coordinate-only GT911
  // frames expose frame-local record slots instead, so callers must not retain
  // those IDs across frames. `count` is capped at MAX_TOUCH_CONTACTS;
  // `reportedCount` keeps the controller's actual count so callers can detect
  // truncation and opt into only the contact counts they support.
  static constexpr uint8_t MAX_TOUCH_CONTACTS = 4;
  struct MultiTouchPoint {
    uint8_t id;
    TouchPoint point;
  };
  struct TouchSnapshot {
    uint8_t count;
    uint8_t reportedCount;
    bool idsStable;
    MultiTouchPoint points[MAX_TOUCH_CONTACTS];
  };

  // True if this board has a touch controller configured.
  bool hasTouch() const;
  // True only while a GT911 controller is present. Other touch controllers
  // retain their existing single-contact contract.
  bool supportsMultiTouch() const;
  // Latest GT911 contacts, capped at MAX_TOUCH_CONTACTS. Gesture consumers
  // should check reportedCount for the exact cardinality they support.
  TouchSnapshot getTouchSnapshot() const;
  // The most recent touch sampled during #update(). valid == false when idle.
  TouchPoint getTouchPoint() const;
  // True while a touch is currently down.
  bool isTouchPressed() const;
  // True if a touch began between the last two #update() calls.
  bool wasTouchPressed() const;
  // True if a touch ended between the last two #update() calls.
  bool wasTouchReleased() const;
  // One-shot tap on release. Returns the original touch-down position
  // normalized to 0..1 in the panel's native frame; false when no tap completed
  // this update.
  bool wasTouchTap(float& nx, float& ny) const;
  // Press edge with the touch-down position normalized in the panel's native
  // frame.
  bool wasTouchPressedAt(float& nx, float& ny) const;
  // True while the current touch is still a tap candidate: finger down,
  // movement remains within tap slop. Writes the original touch-down position
  // and held time.
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  // True while a touch is down; writes the CURRENT contact position normalized
  // to 0..1 in the panel's native frame. Unlike #isTouchTapCandidate there is
  // no tap-slop gate — the position follows the moving finger, for drag
  // interactions (sliders). Callers own any threshold/hysteresis they need.
  bool isTouchHeldAt(float& nx, float& ny) const;
  // Duration (ms) of the last touch contact, latched on release.
  unsigned long lastTouchHeldMs() const;
  // Swipe gesture on release. Returns start/end positions normalized in the
  // panel's native frame; callers map orientation and check this before tap.
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  // One-shot 2-4 contact translation gesture. The SDK reports the number of
  // contacts plus their centroid start/end normalized in its panel-native
  // frame; applications opt into the exact counts they support, map display
  // orientation, and decide whether the result is up/down/left/right. Pinches,
  // diagonal motion, delayed gestures, ambiguous contact matching, track-ID
  // replacements, and sequences above MAX_TOUCH_CONTACTS are rejected.
  bool wasMultiTouchSwipe(uint8_t& contactCount, float& nxStart, float& nyStart, float& nxEnd, float& nyEnd,
                          unsigned long& durationMs) const;
  // One-shot two-contact rotation on release. Positive degrees are clockwise
  // and negative degrees counterclockwise in the corrected panel-native frame,
  // normalized to [-180, 180]. The center is the average of the start/end
  // contact centroids, normalized to 0..1. Pinches and sub-threshold turns are
  // rejected, and an accepted rotation cannot also become a translation.
  bool wasMultiTouchRotation(float& degrees, float& nxCenter, float& nyCenter, unsigned long& durationMs) const;
  // One-shot two-contact pinch/spread on release. `scale` is end separation
  // divided by start separation (<1.0 = pinch in / zoom out, >1.0 = spread /
  // zoom in). The center is the average of the start/end contact centroids,
  // normalized to 0..1. Rotations, tiny scale changes, and ambiguous contacts
  // are rejected, and an accepted pinch cannot also become a translation.
  bool wasMultiTouchPinch(float& scale, float& nxCenter, float& nyCenter, unsigned long& durationMs) const;
  // One-shot long-press: fires WHILE the finger is still down, once a
  // stationary contact (within tap slop) has been held TOUCH_LONG_PRESS_MS.
  // Position is the touch-down point, normalized like wasTouchTap. Fires at
  // most once per contact. Detection alone has no side effects; callers that
  // act on it should call suppressTouchContact() so the eventual lift cannot
  // also tap. Cleared each #update().
  bool wasTouchLongPress(float& nx, float& ny) const;
  // Ignore the remainder of the current contact: tap, swipe, release,
  // tap-candidate and held queries report nothing until the finger lifts and
  // the release edge has passed, then a fresh contact is delivered normally.
  // Self-clears; async tap, single-swipe, multi-touch-swipe, and rotation
  // queues are gated by the same latch.
  void suppressTouchContact();
  // True if a screen-touch press/release or standalone capacitive home-key
  // event happened this frame. Coarse touch-input signal (the touch analogue
  // of wasAnyPressed/Released) for resetting idle/sleep timers and restoring
  // CPU frequency. False on non-touch boards.
  bool wasTouchActivity() const;
  // True on the press edge of the GT911 capacitive home key (controllers
  // without one never report it). Cleared each #update().
  bool wasHomeKeyPressed() const;
  // True once on release of a SHORT home-key press (held < the long-press
  // threshold); the primary "home" action. Suppressed when the same hold
  // already fired wasHomeKeyLongPressed(). Cleared each #update().
  bool wasHomeKeyTapped() const;
  // True once when the home key has been held past the long-press threshold
  // (~700 ms), while still down — a hold shortcut (e.g. open the reader menu).
  // Cleared each #update().
  bool wasHomeKeyLongPressed() const;

  // Optional board hook for buttons that aren't direct GPIOs — e.g. a key
  // behind an I2C IO-expander (the LilyGo T5 S3 user button on its PCA9535). It
  // returns a (1<<BTN_*) bitmask that is OR'd into every update(); the board
  // reads its expander, so InputManager itself stays device-agnostic. Default:
  // none.
  using ButtonHook = uint8_t (*)();
  static void setButtonHook(ButtonHook hook) { s_buttonHook = hook; }

  // Boards such as Sticky wire OK/confirm and power/wake to the same GPIO. By
  // default a short click emits CONFIRM and a hold emits POWER. Apps that
  // expose a "short power click sleeps" option can flip short clicks to POWER.
  static void setSharedConfirmPowerShortPressEmitsPower(bool enabled) {
    s_sharedConfirmPowerShortPressEmitsPower = enabled;
  }

  // When enabled, GT911 polling is throttled to GT911_LOW_POWER_POLL_MS instead
  // of every main-loop iteration. Used while the SoC is in its 80 MHz
  // power-saving reading mode to cut the ~20 I2C txn/s touch poll. Touch INT
  // (GPIO10) still wakes the chip; this only slows the host-side poll cadence.
  static void setLowPowerPolling(bool enabled) { s_lowPowerPolling = enabled; }

  // GT911 Sleep mode (datasheet Rev.09 §8.1.d): commands the touch controller
  // to stop scanning (~70–120 µA vs 8 mA normal). While asleep the controller
  // does not ACK I2C, so pollGt911() no-ops and the capacitive home key is
  // unreachable; physical buttons are unaffected. Only boards with a GT911
  // resolved (gt911Addr != 0) are affected — other configurations are no-ops
  // (return false, state untouched).
  //
  // setTouchSleep(true): drives INT low, writes the sleep command (0x05 to
  // 0x8040), then does a single status read. The datasheet no longer publishes
  // the register map, so the opcode follows the Goodix reference-driver
  // convention and MUST be verified on hardware before merging (see
  // docs/design/2026-09-24-gt911-idle-sleep.md §4). A controller that ACKs the
  // post-command read did not enter sleep; state stays awake and the low-power
  // poll throttle keeps applying.
  //
  // wakeTouch(): the wake side. Drives INT high for ~3 ms, returns the pin to
  // input, then waits up to 200 ms (datasheet response deadline) for the status
  // register to respond before returning, so the caller never races an
  // unresponive controller on its first poll.
  //
  // Threading: the sequences execute on the task that owns touch polling
  // (async poll task when armed via beginAsync(), otherwise the app task
  // through update()). setTouchSleep/wakeTouch only post a command and block
  // until the polling task executes it, so Wire and the INT pin always have a
  // single owner. The touchPressed guard is likewise evaluated on that task.
  // Calls must come from a task context that can block (not an ISR).
  bool setTouchSleep(bool asleep);
  bool wakeTouch();
  // True while the GT911 is in Sleep mode (no scanning, no I2C ACK, home key
  // dead). Wakes happen only through wakeTouch().
  bool isTouchAsleep() const {
    return gt911Asleep.load(std::memory_order_acquire);
  }

  // --- Optional background polling -------------------------------------------
  // Spawns a FreeRTOS task that samples the buttons every pollMs and latches
  // each edge into an internal queue. This decouples input from rendering: on
  // e-paper, a slow refresh blocks the app's main loop, so a press that lands
  // mid-refresh is otherwise lost. No-op if already started.
  //
  // When async polling is active the app must NOT sample hardware itself; the
  // task owns the edge state. update() is a NO-OP in async mode (the poll
  // task owns sampling; edges move to the app only through the pending-edge
  // checks wasPressed/wasReleased, the report-only wasAny* checks, and
  // consumeTouchFrame()) — and edge semantics (press AND release, hold
  // machinery, level state) are preserved. Direct pop*() consumers
  // (standalone tools) may still drain the queue.
  void beginAsync(uint8_t taskPriority = 2, uint32_t pollMs = 15, uint8_t queueLen = 32);

  // Event record latched by the async poller: both press and release edges
  // are queued so the app's release-driven grammar survives busy windows.
  static constexpr uint8_t kAsyncEventPress = 0;
  static constexpr uint8_t kAsyncEventRelease = 1;
  struct AsyncInputEvent {
    uint8_t button;  // BTN_* index
    uint8_t kind;    // kAsyncEventPress / kAsyncEventRelease
  };

  // Pop the next latched edge (BTN_* + press/release kind). Returns false
  // when nothing is pending (or async polling was never started). The
  // async-aware update() is a no-op — edges move to the app only via the
  // pending-edge checks (wasPressed/wasReleased) and consumeTouchFrame();
  // direct consumers are standalone tools.
  bool popPress(AsyncInputEvent& ev);

  // POP PROTOCOL (soak-fix7 final): pop the next queued edge and return a
  // handle to it; nullptr means "nothing to consume" — one operation does
  // consume + emptiness check, never blocks, never returns a zeroed or
  // undefined struct. The handle stays valid until the next popEvent()
  // call (single scratch slot — copy what you need). This is the primary
  // consumption API; wasPressed/wasReleased are thin wrappers that walk
  // the stream until they match, with identical exactly-once semantics.
  const InputManager::AsyncInputEvent* popEvent() const;

  // True while the background polling task owns edge sampling. Wait loops
  // that call update() only to keep debounce progressing can skip it in
  // this mode — the poll task samples every pollMs, and update() in async
  // mode is a no-op anyway (edges move only through the checks).
  bool asyncActive() const { return _asyncTask != nullptr; }

  // Pop the next latched touch tap (normalized 0..1 panel-native coordinates,
  // same frame as wasTouchTap). The async task queues every completed tap, so
  // taps that land while the app thread renders or waits are never lost —
  // drain and route them afterwards. Returns false when no tap is pending.
  bool popTouchTap(float& nx, float& ny);

  // Pop the next latched swipe gesture (normalized 0..1 panel-native start/end
  // coordinates, same frame as wasSwipe). Like taps, async polling queues
  // swipes so gestures that complete during e-paper refreshes are not lost.
  bool popSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd);

  // Pop a queued 2-4 contact translation, in the same normalized panel-native
  // coordinate frame as wasMultiTouchSwipe(). It has a dedicated queue so
  // existing popSwipe() consumers never receive multi-touch gestures. The
  // returned count is captured with the queued event and cannot be confused
  // with a later controller frame.
  bool popMultiTouchSwipe(uint8_t& contactCount, float& nxStart, float& nyStart, float& nxEnd, float& nyEnd,
                          unsigned long& durationMs);

  // Pop a queued completed two-contact rotation. Values use the same signed
  // degrees, normalized center, and duration contract as
  // wasMultiTouchRotation().
  bool popMultiTouchRotation(float& degrees, float& nxCenter, float& nyCenter, unsigned long& durationMs);

  // Pop a queued completed two-contact pinch/spread. Values use the same scale,
  // normalized center, and duration contract as wasMultiTouchPinch().
  bool popMultiTouchPinch(float& scale, float& nxCenter, float& nyCenter, unsigned long& durationMs);

  // --- Async-mode consumption model (soak-fix7 final: consume-on-check) ------
  // Checking a button edge CONSUMES it: wasPressed()/wasReleased() drain the
  // async queue into a pending cache and test-and-clear their bit — checked
  // twice, the second read is false. Non-blocking when nothing is queued.
  // Touch/home-key events are the exception: their getters share event bits
  // with classification predicates (one release edge feeds the swipe, tap,
  // AND raw-release classifiers in a single frame), so per-read consumption
  // would break classification — the whole touch frame is consumed
  // atomically by consumeTouchFrame() once per app tick instead. The old
  // frame-boundary pair (beginInputFrame/drain) is a no-op kept for source
  // compatibility; nothing may depend on it.
  void consumeTouchFrame();
  void beginInputFrame();  // no-op (consume-on-check replaced the frame ack)

  // --- Diagnostics -----------------------------------------------------------
  // A live sample of one button-group ADC pin: the raw reading plus the BTN_*
  // it currently classifies as (-1 = no band matched). On the Xteink ADC ladder
  // the six buttons are resistor dividers multiplexed onto two ADC pins
  // (Back/Confirm/Left/Right on group 1, Up/Down on group 2); X3 and X4 share
  // this pinout. A button-test or calibration screen uses this to spot a
  // drifted divider whose reading no longer lands in the band the firmware
  // expects — visible from the raw value regardless of how it classifies.
  struct ButtonAdcSample {
    int pin;     // GPIO sampled (BUTTON_ADC_PIN_1 / BUTTON_ADC_PIN_2)
    int raw;     // raw analogRead() value, or -1 if this board has no ADC ladder
    int button;  // classified BTN_* index, or -1 for no match
  };

  // Sample both button-group ADC pins now (synchronous analogRead). Safe to
  // call alongside async polling. On boards without the Xteink ADC ladder both
  // samples report raw = -1, button = -1.
  void readButtonAdc(ButtonAdcSample& group1, ButtonAdcSample& group2);

 private:
  static ButtonHook s_buttonHook;

  QueueHandle_t _asyncQueue = nullptr;
  QueueHandle_t _asyncTapQueue = nullptr;
  QueueHandle_t _asyncSwipeQueue = nullptr;
  struct QueuedMultiTouchSwipe {
    uint16_t startX;
    uint16_t startY;
    uint16_t endX;
    uint16_t endY;
    uint8_t contactCount;
    uint16_t durationMs;
  };
  QueueHandle_t _asyncMultiTouchSwipeQueue = nullptr;
  struct QueuedMultiTouchRotation {
    float degrees;
    uint16_t centerX;
    uint16_t centerY;
    uint16_t durationMs;
  };
  QueueHandle_t _asyncMultiTouchRotationQueue = nullptr;
  struct QueuedMultiTouchPinch {
    float scale;
    uint16_t centerX;
    uint16_t centerY;
    uint16_t durationMs;
  };
  QueueHandle_t _asyncMultiTouchPinchQueue = nullptr;
  TaskHandle_t _asyncTask = nullptr;
  uint32_t _asyncPollMs = 15;
  static void asyncTaskTrampoline(void* self);
  void asyncPoll();

  // Async-mode edge split (docs/design/2026-09-20-async-input.md §2.1):
  // taskPressedEdges_/taskReleasedEdges_ are the poll task's own registers
  // (written by the real sampling path when it runs there, and by the hold
  // machinery); the app never sees them directly. Async edges flow
  // queue → pendingPressed_/pendingReleased_ (drained on every edge check,
  // UNION — no loss) → consumed bit-by-bit by wasPressed/wasReleased
  // (consume-on-check). In sync builds the real path runs on the app task
  // and publishes its registers directly — historical semantics.

  // The real sampling path's edge registers (task-local on the poll task;
  // the app never sees them directly — it reads the consume-on-check cache
  // and, in sync builds, the published registers below).
  uint8_t taskPressedEdges_ = 0;
  uint8_t taskReleasedEdges_ = 0;

  // Consume-on-check cache: edges popped from the async queue land here
  // and stay until their specific wasPressed/wasReleased check consumes
  // them. COUNTS per button (not bits): two presses of the same button
  // queued before one check deliver two consumable edges (coderabbit
  // review-r7 — a bit cache would merge them and lose the second).
  // mutable: the getters are const and drain on check (single consumer
  // task). Index range: BTN_BACK..BTN_POWER.
  mutable uint8_t pendingPressCount_[BTN_POWER + 1] = {};
  mutable uint8_t pendingReleaseCount_[BTN_POWER + 1] = {};
  // popEvent()'s single scratch slot: the popped event lives here until the
  // next popEvent() call (the handle the API returns points into it).
  mutable AsyncInputEvent popScratch_ = {};

  // Per-CONSUMED-FRAME snapshot of every field the touch/home-key one-shot
  // getters evaluate (events + gating + points/params). The poll task's LIVE
  // flags are NEVER cleared in async mode (their only clear lives in the
  // sync-only block), so the getters must read this snapshot — consumed
  // atomically from touchOneShots_ by every consumeTouchFrame(); without it
  // one tap/swipe/home-key event replayed on every frame (soak replay
  // regression 2026-09-20). Consume-once per tick: the snapshot holds THIS
  // tick's events; an event not yet consumed is still in touchOneShots_ and
  // delivered by the next tick's consume. The snapshot build is events-OR +
  // geometry-last-wins for
  // the (rare) multi-event pop — two interactions inside one pop are rare
  // and the newest geometry wins.
  struct TouchFrameState {
    bool touchPressedEvent = false;
    bool touchReleasedEvent = false;
    bool multiTouchSwipeEvent = false;
    bool multiTouchRotationEvent = false;
    bool multiTouchPinchEvent = false;
    bool touchLongPressEvent = false;
    bool touchHomeKeyEvent = false;
    bool touchHomeKeyTapEvent = false;
    bool touchHomeKeyLongEvent = false;
    bool touchSuppressed = false;
    bool touchMultiContactSequence = false;
    bool touchMovedBeyondTapReleaseSlop = false;
    bool touchPressed = false;
    unsigned long lastTouchHeldDurationMs = 0;
    TouchPoint touchDownPoint = {false, 0, 0, 0};
    TouchPoint touchUpPoint = {false, 0, 0, 0};
    uint8_t multiTouchSwipeContactCount = 0;
    uint16_t multiTouchSwipeStartX = 0;
    uint16_t multiTouchSwipeStartY = 0;
    uint16_t multiTouchSwipeEndX = 0;
    uint16_t multiTouchSwipeEndY = 0;
    uint16_t multiTouchSwipeDurationMs = 0;
    float multiTouchRotationDegrees = 0.0f;
    uint16_t multiTouchRotationCenterX = 0;
    uint16_t multiTouchRotationCenterY = 0;
    uint16_t multiTouchRotationDurationMs = 0;
    float multiTouchPinchScale = 1.0f;
    uint16_t multiTouchPinchCenterX = 0;
    uint16_t multiTouchPinchCenterY = 0;
    uint16_t multiTouchPinchDurationMs = 0;

    void mergeFrom(const TouchFrameState& live) {
      touchPressedEvent |= live.touchPressedEvent;
      touchReleasedEvent |= live.touchReleasedEvent;
      multiTouchSwipeEvent |= live.multiTouchSwipeEvent;
      multiTouchRotationEvent |= live.multiTouchRotationEvent;
      multiTouchPinchEvent |= live.multiTouchPinchEvent;
      touchLongPressEvent |= live.touchLongPressEvent;
      touchHomeKeyEvent |= live.touchHomeKeyEvent;
      touchHomeKeyTapEvent |= live.touchHomeKeyTapEvent;
      touchHomeKeyLongEvent |= live.touchHomeKeyLongEvent;
      // Gating + geometry: the newest interaction's context.
      touchSuppressed = live.touchSuppressed;
      touchMultiContactSequence = live.touchMultiContactSequence;
      touchMovedBeyondTapReleaseSlop = live.touchMovedBeyondTapReleaseSlop;
      touchPressed = live.touchPressed;
      lastTouchHeldDurationMs = live.lastTouchHeldDurationMs;
      touchDownPoint = live.touchDownPoint;
      touchUpPoint = live.touchUpPoint;
      multiTouchSwipeContactCount = live.multiTouchSwipeContactCount;
      multiTouchSwipeStartX = live.multiTouchSwipeStartX;
      multiTouchSwipeStartY = live.multiTouchSwipeStartY;
      multiTouchSwipeEndX = live.multiTouchSwipeEndX;
      multiTouchSwipeEndY = live.multiTouchSwipeEndY;
      multiTouchSwipeDurationMs = live.multiTouchSwipeDurationMs;
      multiTouchRotationDegrees = live.multiTouchRotationDegrees;
      multiTouchRotationCenterX = live.multiTouchRotationCenterX;
      multiTouchRotationCenterY = live.multiTouchRotationCenterY;
      multiTouchRotationDurationMs = live.multiTouchRotationDurationMs;
      multiTouchPinchScale = live.multiTouchPinchScale;
      multiTouchPinchCenterX = live.multiTouchPinchCenterX;
      multiTouchPinchCenterY = live.multiTouchPinchCenterY;
      multiTouchPinchDurationMs = live.multiTouchPinchDurationMs;
    }
  };  // TouchFrameState

  // The app-task-owned touch snapshot: REPLACED by each consumeTouchFrame()
  // with that tick's consumed events + the poll task's current context.
  // Written only by the consumer task, so plain members (no latch needed).
  TouchFrameState poppedTouch_{};

  // The poll task's touch/home-key one-shot EVENTS as an atomic bitmask (the
  // app latch carries their per-frame snapshot above; the events themselves
  // are produced cross-task and must not race). Set sites fetch_or; the
  // frame drain consumes with exchange(0) — atomic ack, so an event is
  // delivered exactly once per drained frame and never replays.
  enum : uint16_t {
    kEvTouchPressed = 1u << 0,
    kEvTouchReleased = 1u << 1,
    kEvMultiTouchSwipe = 1u << 2,
    kEvMultiTouchRotation = 1u << 3,
    kEvMultiTouchPinch = 1u << 4,
    kEvTouchLongPress = 1u << 5,
    kEvTouchHomeKey = 1u << 6,
    kEvTouchHomeKeyTap = 1u << 7,
    kEvTouchHomeKeyLong = 1u << 8,
  };
  std::atomic<uint16_t> touchOneShots_{0};

  // Mode-aware field read for the one-shot getters: async mode reads this
  // tick's consumed snapshot (consumeTouchFrame consumed the events once);
  // sync frames read the live members (update() clears them historically).
  template <typename T>
  T touchField(T TouchFrameState::*latched, const T& live) const {
    return _asyncTask != nullptr ? poppedTouch_.*latched : live;
  }

  // Async queue → pending cache (UNION — an edge drained is held until its
  // specific check consumes it; nothing is dropped or duplicated).
  void drainQueueIntoPending() const;
  // Builds the per-tick snapshot: event bits (already consumed from the
  // atomic one-shot mask) + the poll task's current gating/geometry context.
  TouchFrameState liveTouchState(uint16_t events) const;
  bool taskWasPressed(const uint8_t buttonIndex) const;
  bool taskWasReleased(const uint8_t buttonIndex) const;

  int getButtonFromADC(int adcValue, const int ranges[], int numButtons);
  bool isDigitalPressed(int8_t pin) const;
  uint8_t getDigitalState() const;
  void updateConfirmBackHold(unsigned long currentTime);
  void updateConfirmPowerHold(unsigned long currentTime);
  void updateDigitalTwoButton(unsigned long currentTime);
  void applyStateChange(uint8_t state, unsigned long currentTime);

  // Touch backend. Compiled only when FREEINK_CAP_TOUCH is set; dispatches on
  // BoardConfig::ACTIVE.touch.controller (CHSC6x, GT911, or FT5x06/FT6336).
  void beginTouch();
  uint8_t serviceTouch();  // runs the machine; returns synthesized button mask
  void updateTouchFromIrq(unsigned long now,
                          int irqRaw);  // CHSC6x I2C poll + touch-bit gate
  void pollGt911(unsigned long now);    // GT911 polled read
  void beginFt5x06();
  void pollFt5x06(unsigned long now);
  bool ft5x06WriteReg(uint8_t reg, uint8_t value);
  bool ft5x06ReadReg(uint8_t reg, uint8_t* buf, uint8_t len);
  bool readChsc6xPoint(TouchPoint& point);
  bool decodeChsc6xFrame(const uint8_t *data, size_t len,
                         TouchPoint &point) const;
  uint16_t mapTouchAxis(uint16_t raw, uint16_t rawMin, uint16_t rawMax,
                        uint16_t outMax) const;
  void beginGt911();
  bool gt911ReadReg(uint16_t reg, uint8_t *buf, uint8_t len);
  void gt911ClearStatus();
  // Sleep/wake command channel. Sleep/wake sequences drive Wire and the INT
  // pin, which only the polling task may touch (async poll task when armed,
  // otherwise the app task through update()). Public setters post a request
  // here and wait for the outcome; pollGt911() services the posted command at
  // its top, before any poll-path state.
  static constexpr uint8_t GT911_TOUCH_CMD_SLEEP = 1;
  static constexpr uint8_t GT911_TOUCH_CMD_WAKE = 2;
  static constexpr unsigned long GT911_TOUCH_CMD_TIMEOUT_MS = 1000;
  void serviceGt911TouchCmd();
  void enterGt911Sleep();
  bool exitGt911Sleep();
  void beginFt6336u();
  void pollFt6336u(unsigned long now);
  void beginGslx680();
  void pollGslx680(unsigned long now);
  bool gslWrite(uint8_t reg, const uint8_t* data, uint8_t len);
  bool gslRead(uint8_t reg, uint8_t* buf, uint8_t len);
  bool gslUploadFirmware();

  enum class MultiTouchGestureState : uint8_t { Idle, Tracking, Blocked };
  struct TrackedTouchContact {
    uint8_t id;
    TouchPoint start;
    TouchPoint last;
  };
  void updateMultiTouchGesture(const TouchSnapshot& snapshot, unsigned long now);
  void startMultiTouchGesture(const TouchSnapshot& snapshot, unsigned long now);
  void blockMultiTouchGesture();
  void finishMultiTouchGesture(unsigned long now);
  void resetMultiTouchGesture();
  void cancelMultiTouchGesture();
  bool matchMultiTouchSnapshot(const TouchSnapshot& snapshot);
  bool expandMultiTouchGesture(const TouchSnapshot& snapshot, unsigned long now);
  bool findContactAssignment(const TouchSnapshot& snapshot, uint8_t trackedCount,
                             uint8_t assignment[MAX_TOUCH_CONTACTS]) const;
  bool isTrackedContact(const MultiTouchPoint& point) const;
  bool hasStableTranslationGeometry() const;
  bool hasEligibleRotationScale() const;
  bool isMultiTouchTranslation(unsigned long now) const;
  bool classifyMultiTouchRotation(unsigned long now);
  bool classifyMultiTouchPinch(unsigned long now);
  void normalizeTouchPoint(uint16_t x, uint16_t y, float& nx, float& ny) const;

  uint8_t currentState;
  uint8_t lastState;
  uint8_t pressedEvents;
  uint8_t releasedEvents;
  unsigned long lastDebounceTime;
  unsigned long buttonPressStart;
  unsigned long buttonPressFinish;
  unsigned long powerButtonPressStart;
  unsigned long powerButtonPressFinish;
  unsigned long confirmBackPressStart;
  bool confirmBackPhysicalPressed;
  bool confirmBackLongPressActive;
  unsigned long confirmPowerPressStart;
  bool confirmPowerPhysicalPressed;
  bool confirmPowerLongPressActive;
  uint8_t twoButtonPhysicalState;
  unsigned long twoButtonPressStart;
  bool twoButtonLongPressActive;

  bool touchDataEnabled = false; // I2C up, controller present
  uint8_t gt911Addr = 0;         // resolved GT911 address (0 until probed)
  unsigned long touchIrqPulseUntil =
      0;                         // synthesized-confirm window after a press
  unsigned long touchReadAt = 0; // next scheduled I2C poll
  unsigned long lastGt911Poll =
      0; // last GT911 I2C poll timestamp (low-power throttle)
  std::atomic<bool> gt911Asleep{
      false}; // GT911 in Sleep mode: no polling, no home key
  unsigned long gt911SleepEnteredAt =
      0; // sleep command sent; gate wake ≥ 58 ms after (§8.1.d)
  unsigned long touchReleaseAt = 0;
  bool touchPressed = false;
  bool touchPressedEvent = false;
  bool touchReleasedEvent = false;
  bool touchHomeKeyEvent = false;  // GT911 capacitive home key, press edge
  bool touchHomeKeyDown = false;
  bool touchHomeKeyTapEvent = false;   // short-press release edge (one-shot)
  bool touchHomeKeyLongEvent = false;  // held past the long-press threshold (one-shot)
  bool touchHomeKeyLongFired = false;  // latched for the current hold so long
                                       // fires once and suppresses the tap
  unsigned long touchHomeKeyDownAt = 0;
  static constexpr unsigned long HOME_KEY_LONG_PRESS_MS = 700;
  TouchPoint touchPoint = {false, 0, 0, 0};
  TouchSnapshot touchSnapshot{};
  MultiTouchGestureState multiTouchGestureState = MultiTouchGestureState::Idle;
  TrackedTouchContact multiTouchContacts[MAX_TOUCH_CONTACTS] = {};
  uint8_t trackedTouchContactCount = 0;
  bool multiTouchRotationEligible = false;  // latched false if any frame leaves the allowed scale band
  bool touchMultiContactSequence = false;   // suppresses single-contact classifiers until full release
  bool multiTouchSwipeEvent = false;
  uint8_t multiTouchSwipeContactCount = 0;
  uint16_t multiTouchSwipeStartX = 0;
  uint16_t multiTouchSwipeStartY = 0;
  uint16_t multiTouchSwipeEndX = 0;
  uint16_t multiTouchSwipeEndY = 0;
  uint16_t multiTouchSwipeDurationMs = 0;
  bool multiTouchRotationEvent = false;
  float multiTouchRotationDegrees = 0.0f;
  uint16_t multiTouchRotationCenterX = 0;
  uint16_t multiTouchRotationCenterY = 0;
  uint16_t multiTouchRotationDurationMs = 0;
  bool multiTouchPinchEvent = false;
  float multiTouchPinchScale = 1.0f;
  uint16_t multiTouchPinchCenterX = 0;
  uint16_t multiTouchPinchCenterY = 0;
  uint16_t multiTouchPinchDurationMs = 0;
  TouchPoint touchDownPoint = {false, 0, 0, 0};  // first sample of the current contact (tap routing)
  TouchPoint touchUpPoint = {false, 0, 0, 0};    // last sample before release (swipe routing)
  unsigned long lastTouchHeldDurationMs = 0;     // contact duration, latched at release
  bool touchMovedBeyondTapSlop = false;          // cancels stationary hold/long-press classification
  bool touchMovedBeyondTapReleaseSlop = false;   // cancels tap-on-release once motion reaches swipe distance
  bool touchLongPressEvent = false;              // one-shot, mirrors touchHomeKeyLongEvent
  bool touchLongPressFired = false;              // latched for the current contact so long-press fires once
  bool touchSuppressed = false;                  // suppressTouchContact() latch; holds through
                                                 // the release-edge frame, cleared in
                                                 // serviceTouch() once the contact is over

  static constexpr int NUM_BUTTONS_1 = 4;
  static const int ADC_RANGES_1[];

  static constexpr int NUM_BUTTONS_2 = 2;
  static const int ADC_RANGES_2[];

  static constexpr int ADC_NO_BUTTON = 3900;
  static constexpr unsigned long DEBOUNCE_DELAY = 5;
  static constexpr unsigned long CONFIRM_BACK_HOLD_MS = 650;
  static constexpr unsigned long CONFIRM_POWER_HOLD_MS = 400;
  static constexpr unsigned long TWO_BUTTON_HOLD_MS = 650;

  // Touch timing / protocol constants (ported from the Murphy M3 CHSC6x
  // driver).
  static constexpr unsigned long TOUCH_IRQ_PULSE_MS = 120;   // release hold-over after last valid read
  static constexpr unsigned long TOUCH_SAMPLE_DELAY_MS = 8;  // I2C poll cadence
  static constexpr int TOUCH_TAP_SLOP_PX = 28;
  static constexpr int TOUCH_SWIPE_MIN_PX = 60;
  static constexpr int TOUCH_TAP_RELEASE_SLOP_PX = TOUCH_SWIPE_MIN_PX - 1;
  static constexpr unsigned long TOUCH_SWIPE_MAX_MS = 700;
  static constexpr unsigned long TOUCH_MULTI_SWIPE_MAX_MS = 2000;
  static constexpr int TOUCH_MULTI_CONTACT_SEPARATION_SLOP_PX = 45;
  static constexpr int64_t TOUCH_CONTACT_ASSIGNMENT_AMBIGUITY_PX_SQ = 64;
  static constexpr unsigned long TOUCH_LONG_PRESS_MS = 500;  // shorter than HOME_KEY_LONG_PRESS_MS: a screen hold has
                                                             // no button travel to absorb
  static constexpr uint8_t TOUCH_READ_COMMAND = 0x00;
  static constexpr uint8_t TOUCH_FRAME_SIZE = 16;

  // Host-side GT911 poll cadence while in 80 MHz power-saving reading mode.
  // Touch INT (GPIO10) still wakes the SoC; this only slows the I2C re-poll so
  // we aren't hammering the bus ~20x/s when nothing is happening.
  static constexpr unsigned long GT911_LOW_POWER_POLL_MS = 100;

  static const char *BUTTON_NAMES[];
  static bool s_sharedConfirmPowerShortPressEmitsPower;
  static bool s_lowPowerPolling;
  // GT911 sleep/wake request channel (see serviceGt911TouchCmd); in-flight
  // command stays visible to the caller until the polling task clears it.
  static std::atomic<uint8_t> s_gt911TouchCmd;
};
