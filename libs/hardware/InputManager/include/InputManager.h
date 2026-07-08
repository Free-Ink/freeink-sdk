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
#include <freertos/queue.h>
#include <freertos/task.h>

class InputManager {
 public:
  InputManager();
  void begin();
  uint8_t getState();

  // Call regularly from the main loop to update button and touch edge state.
  void update();

  // Level state from the last update().
  bool isPressed(uint8_t buttonIndex) const;

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
  // sleep-sliced idle loop) should re-poll quickly while this is set — otherwise
  // a press shorter than the poll period lands in a single sample and is dropped.
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

  // Pins. POWER_BUTTON_PIN stays constexpr (consumers reference it in pin-config
  // contexts) and is bound to the build's default device; the input code reads
  // the runtime-active power pin internally so multi-device builds stay correct.
  static constexpr int BUTTON_ADC_PIN_1 = 1;
  static constexpr int BUTTON_ADC_PIN_2 = 2;
  static constexpr int POWER_BUTTON_PIN = BoardConfig::DEFAULT_DEVICE.input.power;

  bool isPowerButtonPressed() const;

  static const char* getButtonName(uint8_t buttonIndex);

  // --- Capacitive touch (inert unless BoardConfig::ACTIVE.touch is configured) ---
  struct TouchPoint {
    bool valid;
    uint16_t x;
    uint16_t y;
    unsigned long timestamp;
  };

  // True if this board has a touch controller configured.
  bool hasTouch() const;
  // The most recent touch sampled during #update(). valid == false when idle.
  TouchPoint getTouchPoint() const;
  // True while a touch is currently down.
  bool isTouchPressed() const;
  // True if a touch began between the last two #update() calls.
  bool wasTouchPressed() const;
  // True if a touch ended between the last two #update() calls.
  bool wasTouchReleased() const;
  // One-shot tap on release. Returns the original touch-down position normalized
  // to 0..1 in the panel's native frame; false when no tap completed this update.
  bool wasTouchTap(float& nx, float& ny) const;
  // Press edge with the touch-down position normalized in the panel's native frame.
  bool wasTouchPressedAt(float& nx, float& ny) const;
  // True while the current touch is still a tap candidate: finger down, movement
  // remains within tap slop. Writes the original touch-down position and held time.
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  // True while a touch is down; writes the CURRENT contact position normalized to
  // 0..1 in the panel's native frame. Unlike #isTouchTapCandidate there is no
  // tap-slop gate — the position follows the moving finger, for drag interactions
  // (sliders). Callers own any threshold/hysteresis they need.
  bool isTouchHeldAt(float& nx, float& ny) const;
  // Duration (ms) of the last touch contact, latched on release.
  unsigned long lastTouchHeldMs() const;
  // Swipe gesture on release. Returns start/end positions normalized in the
  // panel's native frame; callers map orientation and check this before tap.
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  // True if a touch press or release happened this frame. Coarse "the user touched
  // the screen" signal (the touch analogue of wasAnyPressed/Released) for resetting
  // idle/sleep timers and restoring CPU frequency. False on non-touch boards.
  bool wasTouchActivity() const;
  // True on the press edge of the GT911 capacitive home key (controllers without
  // one never report it). Cleared each #update().
  bool wasHomeKeyPressed() const;

  // Optional board hook for buttons that aren't direct GPIOs — e.g. a key behind
  // an I2C IO-expander (the LilyGo T5 S3 user button on its PCA9535). It returns
  // a (1<<BTN_*) bitmask that is OR'd into every update(); the board reads its
  // expander, so InputManager itself stays device-agnostic. Default: none.
  using ButtonHook = uint8_t (*)();
  static void setButtonHook(ButtonHook hook) { s_buttonHook = hook; }

  // Boards such as Sticky wire OK/confirm and power/wake to the same GPIO. By
  // default a short click emits CONFIRM and a hold emits POWER. Apps that expose
  // a "short power click sleeps" option can flip short clicks to POWER.
  static void setSharedConfirmPowerShortPressEmitsPower(bool enabled) {
    s_sharedConfirmPowerShortPressEmitsPower = enabled;
  }

  // --- Optional background polling -------------------------------------------
  // Spawns a FreeRTOS task that samples the buttons every pollMs and latches
  // each press edge (a BTN_* index) into an internal queue. This decouples input
  // from rendering: on e-paper, a slow refresh blocks the app's main loop, so a
  // press that lands mid-refresh is otherwise lost — the task keeps sampling
  // (refresh busy-waits yield via delay()) and the app drains presses with
  // popPress() afterward. No-op if already started.
  //
  // When async polling is active the app must NOT call update()/wasPressed()
  // itself; the task owns the edge state. Drain with popPress() instead.
  void beginAsync(uint8_t taskPriority = 2, uint32_t pollMs = 15, uint8_t queueLen = 32);

  // Pop the next latched button index (BTN_*) into `button`. Returns false when
  // no press is pending (or async polling was never started).
  bool popPress(uint8_t& button);

  // Pop the next latched touch tap (normalized 0..1 panel-native coordinates,
  // same frame as wasTouchTap). The async task queues every completed tap, so
  // taps that land while the app thread renders or waits are never lost —
  // drain and route them afterwards. Returns false when no tap is pending.
  bool popTouchTap(float& nx, float& ny);

  // Pop the next latched swipe gesture (normalized 0..1 panel-native start/end
  // coordinates, same frame as wasSwipe). Like taps, async polling queues swipes
  // so gestures that complete during e-paper refreshes are not lost.
  bool popSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd);

  // --- Diagnostics -----------------------------------------------------------
  // A live sample of one button-group ADC pin: the raw reading plus the BTN_*
  // it currently classifies as (-1 = no band matched). On the Xteink ADC ladder
  // the six buttons are resistor dividers multiplexed onto two ADC pins
  // (Back/Confirm/Left/Right on group 1, Up/Down on group 2); X3 and X4 share
  // this pinout. A button-test or calibration screen uses this to spot a drifted
  // divider whose reading no longer lands in the band the firmware expects —
  // visible from the raw value regardless of how it classifies.
  struct ButtonAdcSample {
    int pin;     // GPIO sampled (BUTTON_ADC_PIN_1 / BUTTON_ADC_PIN_2)
    int raw;     // raw analogRead() value, or -1 if this board has no ADC ladder
    int button;  // classified BTN_* index, or -1 for no match
  };

  // Sample both button-group ADC pins now (synchronous analogRead). Safe to call
  // alongside async polling. On boards without the Xteink ADC ladder both
  // samples report raw = -1, button = -1.
  void readButtonAdc(ButtonAdcSample& group1, ButtonAdcSample& group2);

 private:
  static ButtonHook s_buttonHook;

  QueueHandle_t _asyncQueue = nullptr;
  QueueHandle_t _asyncTapQueue = nullptr;
  QueueHandle_t _asyncSwipeQueue = nullptr;
  TaskHandle_t _asyncTask = nullptr;
  uint32_t _asyncPollMs = 15;
  static void asyncTaskTrampoline(void* self);
  void asyncPoll();

  int getButtonFromADC(int adcValue, const int ranges[], int numButtons);
  bool isDigitalPressed(int8_t pin) const;
  uint8_t getDigitalState() const;
  void updateConfirmBackHold(unsigned long currentTime);
  void updateConfirmPowerHold(unsigned long currentTime);
  void applyStateChange(uint8_t state, unsigned long currentTime);

  // Touch backend. Compiled only when FREEINK_CAP_TOUCH is set; dispatches on
  // BoardConfig::ACTIVE.touch.controller (CHSC6x IRQ-driven, GT911 polled).
  void beginTouch();
  uint8_t serviceTouch();                                   // runs the machine; returns synthesized button mask
  void updateTouchFromIrq(unsigned long now, int irqRaw);   // CHSC6x I2C poll + touch-bit gate
  void pollGt911(unsigned long now);                        // GT911 polled read
  bool readChsc6xPoint(TouchPoint& point);
  bool decodeChsc6xFrame(const uint8_t* data, size_t len, TouchPoint& point) const;
  uint16_t mapTouchAxis(uint16_t raw, uint16_t rawMin, uint16_t rawMax, uint16_t outMax) const;
  void beginGt911();
  bool gt911ReadReg(uint16_t reg, uint8_t* buf, uint8_t len);
  void gt911ClearStatus();

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

  bool touchDataEnabled = false;      // I2C up, controller present
  uint8_t gt911Addr = 0;              // resolved GT911 address (0 until probed)
  unsigned long touchIrqPulseUntil = 0;  // synthesized-confirm window after a press
  unsigned long touchReadAt = 0;         // next scheduled I2C poll
  unsigned long touchReleaseAt = 0;
  bool touchPressed = false;
  bool touchPressedEvent = false;
  bool touchReleasedEvent = false;
  bool touchHomeKeyEvent = false;  // GT911 capacitive home key, press edge
  bool touchHomeKeyDown = false;
  TouchPoint touchPoint = {false, 0, 0, 0};
  TouchPoint touchDownPoint = {false, 0, 0, 0};  // first sample of the current contact (tap routing)
  TouchPoint touchUpPoint = {false, 0, 0, 0};    // last sample before release (swipe routing)
  unsigned long lastTouchHeldDurationMs = 0;     // contact duration, latched at release
  bool touchMovedBeyondTapSlop = false;          // suppresses tap activation after a drag/scroll

  static constexpr int NUM_BUTTONS_1 = 4;
  static const int ADC_RANGES_1[];

  static constexpr int NUM_BUTTONS_2 = 2;
  static const int ADC_RANGES_2[];

  static constexpr int ADC_NO_BUTTON = 3900;
  static constexpr unsigned long DEBOUNCE_DELAY = 5;
  static constexpr unsigned long CONFIRM_BACK_HOLD_MS = 650;
  static constexpr unsigned long CONFIRM_POWER_HOLD_MS = 400;

  // Touch timing / protocol constants (ported from the Murphy M3 CHSC6x driver).
  static constexpr unsigned long TOUCH_IRQ_PULSE_MS = 120;   // release hold-over after last valid read
  static constexpr unsigned long TOUCH_SAMPLE_DELAY_MS = 8;  // I2C poll cadence
  static constexpr int TOUCH_TAP_SLOP_PX = 28;
  static constexpr int TOUCH_SWIPE_MIN_PX = 60;
  static constexpr unsigned long TOUCH_SWIPE_MAX_MS = 700;
  static constexpr uint8_t TOUCH_READ_COMMAND = 0x00;
  static constexpr uint8_t TOUCH_FRAME_SIZE = 16;

  static const char* BUTTON_NAMES[];
  static bool s_sharedConfirmPowerShortPressEmitsPower;
};
