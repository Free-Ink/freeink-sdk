#include "InputManager.h"

#include <algorithm>

#include "MultiTouchGestureMath.h"

#if FREEINK_CAP_TOUCH
#include <Wire.h>
#include <driver/gpio.h>
#if FREEINK_DEVICE_MURPHY_M4
#include <driver/i2c_master.h>
#include <esp_rom_sys.h>
#endif
#endif
#if FREEINK_DEVICE_PAPERMONO
#include <PaperMonoBoard.h>
#endif
#if defined(TOUCH_PROBE_DEBUG)
#include <esp_rom_sys.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#endif

// Recorded ADC values from real devices
// BACK CONF LEFT RGHT   UP DOWN
// 3597 2760 1530    6 2300    6
// 3470 2666 1480    6 2222    5
// 3470 2655 1470    3 2205    3
//
// Averages
// BACK CONF LEFT RGHT   UP DOWN
// 3512 2694 1493    5 2242    5
//
// Setup ranges, if ADC value is between value `i` and `i + 1`, button `i` is
// being pressed. These ranges are based on real world values above, and are
// much more tolerant of different devices than a fixed threshold check. They
// are calculated by taking the midpoint of the pairs of averaged values above.
const int InputManager::ADC_RANGES_1[] = {ADC_NO_BUTTON, 3100, 2090, 750, INT32_MIN};
const int InputManager::ADC_RANGES_2[] = {ADC_NO_BUTTON, 1120, INT32_MIN};
const char* InputManager::BUTTON_NAMES[] = {"Back", "Confirm", "Left", "Right", "Up", "Down", "Power"};

namespace {
int absInt(const int value) { return value < 0 ? -value : value; }

#if defined(TOUCH_PROBE_DEBUG)
void touchDebugPrintf(const char* format, ...) {
  char buf[192];
  va_list args;
  va_start(args, format);
  const int len = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (len < 0) return;
  const size_t n = strnlen(buf, sizeof(buf));
#if FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_USB_CDC_WRITE
  Serial.write(reinterpret_cast<const uint8_t*>(buf), n);
#elif FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF
  esp_rom_printf("%s", buf);
#else
  if (Serial) {
    Serial.print(buf);
  }
#endif
}
#endif
}  // namespace

InputManager::InputManager()
    : currentState(0),
      lastState(0),
      pressedEvents(0),
      releasedEvents(0),
      lastDebounceTime(0),
      buttonPressStart(0),
      buttonPressFinish(0),
      powerButtonPressStart(0),
      powerButtonPressFinish(0),
      confirmBackPressStart(0),
      confirmBackPhysicalPressed(false),
      confirmBackLongPressActive(false),
      confirmPowerPressStart(0),
      confirmPowerPhysicalPressed(false),
      confirmPowerLongPressActive(false),
      twoButtonPhysicalState(0),
      twoButtonPressStart(0),
      twoButtonLongPressActive(false) {}

void InputManager::begin() {
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder) {
    pinMode(BUTTON_ADC_PIN_1, INPUT);
    pinMode(BUTTON_ADC_PIN_2, INPUT);
    pinMode(BoardConfig::ACTIVE.input.power, BoardConfig::ACTIVE.input.powerActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
    analogSetAttenuation(ADC_11db);
    beginTouch();
    return;
  }

  const int8_t pins[] = {BoardConfig::ACTIVE.input.back, BoardConfig::ACTIVE.input.confirm,
                         BoardConfig::ACTIVE.input.left, BoardConfig::ACTIVE.input.right,
                         BoardConfig::ACTIVE.input.up,   BoardConfig::ACTIVE.input.down,
                         BoardConfig::ACTIVE.input.power};
  for (const int8_t pin : pins) {
    if (pin >= 0) {
      pinMode(pin, INPUT_PULLUP);
    }
  }
  beginTouch();
}

int InputManager::getButtonFromADC(const int adcValue, const int ranges[], const int numButtons) {
  for (int i = 0; i < numButtons; i++) {
    if (ranges[i + 1] < adcValue && adcValue <= ranges[i]) {
      return i;
    }
  }

  return -1;
}

void InputManager::readButtonAdc(ButtonAdcSample& group1, ButtonAdcSample& group2) {
  group1 = {BUTTON_ADC_PIN_1, -1, -1};
  group2 = {BUTTON_ADC_PIN_2, -1, -1};
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    return;
  }

  group1.raw = analogRead(BUTTON_ADC_PIN_1);
  group1.button = getButtonFromADC(group1.raw, ADC_RANGES_1, NUM_BUTTONS_1);

  group2.raw = analogRead(BUTTON_ADC_PIN_2);
  const int b2 = getButtonFromADC(group2.raw, ADC_RANGES_2, NUM_BUTTONS_2);
  group2.button = b2 >= 0 ? b2 + 4 : -1;  // map group-2 local 0/1 to BTN_UP / BTN_DOWN
}

uint8_t InputManager::getState() {
  uint8_t state = 0;

  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    state = getDigitalState();
    state |= serviceTouch();                    // run the touch machine; OR any synthesized button
    if (s_buttonHook) state |= s_buttonHook();  // board buttons (e.g. I2C expander)
    return state;
  }

  // Read GPIO1 buttons
  const int adcValue1 = analogRead(BUTTON_ADC_PIN_1);
  const int button1 = getButtonFromADC(adcValue1, ADC_RANGES_1, NUM_BUTTONS_1);
  if (button1 >= 0) {
    state |= (1 << button1);
  }

  // Read GPIO2 buttons
  const int adcValue2 = analogRead(BUTTON_ADC_PIN_2);
  const int button2 = getButtonFromADC(adcValue2, ADC_RANGES_2, NUM_BUTTONS_2);
  if (button2 >= 0) {
    state |= (1 << (button2 + 4));
  }

  // Read power button (polarity per board; X4 active-LOW, de-link active-HIGH)
  const int powerActiveLevel = BoardConfig::ACTIVE.input.powerActiveHigh ? HIGH : LOW;
  if (digitalRead(BoardConfig::ACTIVE.input.power) == powerActiveLevel) {
    state |= (1 << BTN_POWER);
  }

  state |= serviceTouch();
  if (s_buttonHook) state |= s_buttonHook();  // board buttons (e.g. I2C expander)
  return state;
}

InputManager::ButtonHook InputManager::s_buttonHook = nullptr;

void InputManager::beginAsync(const uint8_t taskPriority, const uint32_t pollMs, const uint8_t queueLen) {
  if (_asyncTask) return;  // already running
  _asyncPollMs = pollMs;
  _asyncQueue = xQueueCreate(queueLen, sizeof(uint8_t));
  if (!_asyncQueue) return;
  _asyncTapQueue = xQueueCreate(queueLen, sizeof(float) * 2);
  _asyncSwipeQueue = xQueueCreate(queueLen, sizeof(float) * 4);
  _asyncMultiTouchSwipeQueue = xQueueCreate(queueLen, sizeof(QueuedMultiTouchSwipe));
  _asyncMultiTouchRotationQueue = xQueueCreate(queueLen, sizeof(QueuedMultiTouchRotation));
  xTaskCreate(asyncTaskTrampoline, "fi_input", 4096, this, taskPriority, &_asyncTask);
}

void InputManager::asyncTaskTrampoline(void* self) { static_cast<InputManager*>(self)->asyncPoll(); }

void InputManager::asyncPoll() {
  static const uint8_t kButtons[] = {BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER};
  for (;;) {
    update();
    for (const uint8_t b : kButtons) {
      if (wasPressed(b)) xQueueSend(_asyncQueue, &b, 0);
    }
    float tap[2];
    if (_asyncTapQueue && wasTouchTap(tap[0], tap[1])) {
      xQueueSend(_asyncTapQueue, tap, 0);
    }
    float swipe[4];
    if (_asyncSwipeQueue && wasSwipe(swipe[0], swipe[1], swipe[2], swipe[3])) {
      xQueueSend(_asyncSwipeQueue, swipe, 0);
    }
    if (_asyncMultiTouchSwipeQueue && multiTouchSwipeEvent && !touchSuppressed) {
      const QueuedMultiTouchSwipe multiTouchSwipe = {multiTouchSwipeStartX,       multiTouchSwipeStartY,
                                                     multiTouchSwipeEndX,         multiTouchSwipeEndY,
                                                     multiTouchSwipeContactCount, multiTouchSwipeDurationMs};
      xQueueSend(_asyncMultiTouchSwipeQueue, &multiTouchSwipe, 0);
    }
    if (_asyncMultiTouchRotationQueue && multiTouchRotationEvent && !touchSuppressed) {
      const QueuedMultiTouchRotation rotation = {multiTouchRotationDegrees, multiTouchRotationCenterX,
                                                 multiTouchRotationCenterY, multiTouchRotationDurationMs};
      xQueueSend(_asyncMultiTouchRotationQueue, &rotation, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(_asyncPollMs));
  }
}

bool InputManager::popPress(uint8_t& button) {
  if (!_asyncQueue) return false;
  return xQueueReceive(_asyncQueue, &button, 0) == pdTRUE;
}

bool InputManager::popTouchTap(float& nx, float& ny) {
  if (!_asyncTapQueue) return false;
  float tap[2];
  if (xQueueReceive(_asyncTapQueue, tap, 0) != pdTRUE) return false;
  nx = tap[0];
  ny = tap[1];
  return true;
}

bool InputManager::popSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) {
  if (!_asyncSwipeQueue) return false;
  float swipe[4];
  if (xQueueReceive(_asyncSwipeQueue, swipe, 0) != pdTRUE) return false;
  nxStart = swipe[0];
  nyStart = swipe[1];
  nxEnd = swipe[2];
  nyEnd = swipe[3];
  return true;
}

bool InputManager::popMultiTouchSwipe(uint8_t& contactCount, float& nxStart, float& nyStart, float& nxEnd, float& nyEnd,
                                      unsigned long& durationMs) {
  if (!_asyncMultiTouchSwipeQueue) return false;
  QueuedMultiTouchSwipe swipe{};
  if (xQueueReceive(_asyncMultiTouchSwipeQueue, &swipe, 0) != pdTRUE) return false;
  contactCount = swipe.contactCount;
  normalizeTouchPoint(swipe.startX, swipe.startY, nxStart, nyStart);
  normalizeTouchPoint(swipe.endX, swipe.endY, nxEnd, nyEnd);
  durationMs = swipe.durationMs;
  return true;
}

bool InputManager::popMultiTouchRotation(float& degrees, float& nxCenter, float& nyCenter, unsigned long& durationMs) {
  if (!_asyncMultiTouchRotationQueue) return false;
  QueuedMultiTouchRotation rotation{};
  if (xQueueReceive(_asyncMultiTouchRotationQueue, &rotation, 0) != pdTRUE) return false;
  degrees = rotation.degrees;
  normalizeTouchPoint(rotation.centerX, rotation.centerY, nxCenter, nyCenter);
  durationMs = rotation.durationMs;
  return true;
}

bool InputManager::isDigitalPressed(const int8_t pin) const { return pin >= 0 && digitalRead(pin) == LOW; }

uint8_t InputManager::getDigitalState() const {
  uint8_t state = 0;

  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmBackHold &&
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    if (isDigitalPressed(BoardConfig::ACTIVE.input.back)) state |= (1 << BTN_BACK);
    if (isDigitalPressed(BoardConfig::ACTIVE.input.confirm)) state |= (1 << BTN_CONFIRM);
  }

  if (isDigitalPressed(BoardConfig::ACTIVE.input.left)) state |= (1 << BTN_LEFT);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.right)) state |= (1 << BTN_RIGHT);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.up)) state |= (1 << BTN_UP);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.down)) state |= (1 << BTN_DOWN);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.power) &&
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmBackHold &&
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    state |= (1 << BTN_POWER);
  }

  return state;
}

void InputManager::applyStateChange(const uint8_t state, const unsigned long currentTime) {
  pressedEvents = state & ~currentState;
  releasedEvents = currentState & ~state;

  if (pressedEvents > 0 && currentState == 0) {
    buttonPressStart = currentTime;
  }

  if (releasedEvents > 0 && state == 0) {
    buttonPressFinish = currentTime;
  }

  if (pressedEvents & (1 << BTN_POWER)) {
    powerButtonPressStart = currentTime;
  }

  if (releasedEvents & (1 << BTN_POWER)) {
    powerButtonPressFinish = currentTime;
  }

  currentState = state;
  // Keep lastState in sync with the committed state so isDebouncePending() is
  // meaningful on every input style. A no-op for the debounced ADC path (state
  // already equals lastState at commit time), but the hold-style updates call
  // applyStateChange() directly without ever sampling through the debounce.
  lastState = state;
}

void InputManager::updateConfirmBackHold(const unsigned long currentTime) {
  const bool pressed = isDigitalPressed(BoardConfig::ACTIVE.input.confirm);
  const uint8_t nonSharedState = getDigitalState();
  bool emitConfirmClick = false;

  if (pressed && !confirmBackPhysicalPressed) {
    confirmBackPhysicalPressed = true;
    confirmBackLongPressActive = false;
    confirmBackPressStart = currentTime;
  }

  uint8_t nextState = nonSharedState;
  if (pressed && currentTime - confirmBackPressStart >= CONFIRM_BACK_HOLD_MS) {
    confirmBackLongPressActive = true;
    nextState |= (1 << BTN_BACK);
  }

  if (!pressed && confirmBackPhysicalPressed) {
    confirmBackPhysicalPressed = false;
    if (!confirmBackLongPressActive) {
      emitConfirmClick = true;
      buttonPressStart = confirmBackPressStart;
      buttonPressFinish = currentTime;
    }
    confirmBackLongPressActive = false;
  }

  applyStateChange(nextState, currentTime);

  if (emitConfirmClick) {
    pressedEvents |= (1 << BTN_CONFIRM);
    releasedEvents |= (1 << BTN_CONFIRM);
  }
}

void InputManager::updateConfirmPowerHold(const unsigned long currentTime) {
  const int8_t sharedPin =
      BoardConfig::ACTIVE.input.confirm >= 0 ? BoardConfig::ACTIVE.input.confirm : BoardConfig::ACTIVE.input.power;
  const bool pressed = isDigitalPressed(sharedPin);
  uint8_t nonSharedState = getDigitalState();
  nonSharedState |= serviceTouch();
  if (s_buttonHook) nonSharedState |= s_buttonHook();
  bool emitConfirmClick = false;

  if (pressed && !confirmPowerPhysicalPressed) {
    confirmPowerPhysicalPressed = true;
    confirmPowerLongPressActive = false;
    confirmPowerPressStart = currentTime;
  }

  uint8_t nextState = nonSharedState;
  if (pressed && s_sharedConfirmPowerShortPressEmitsPower) {
    nextState |= (1 << BTN_POWER);
  } else if (pressed && currentTime - confirmPowerPressStart >= CONFIRM_POWER_HOLD_MS) {
    confirmPowerLongPressActive = true;
    nextState |= (1 << BTN_POWER);
  }

  if (!pressed && confirmPowerPhysicalPressed) {
    confirmPowerPhysicalPressed = false;
    if (!confirmPowerLongPressActive) {
      if (!s_sharedConfirmPowerShortPressEmitsPower) {
        emitConfirmClick = true;
      }
      buttonPressStart = confirmPowerPressStart;
      buttonPressFinish = currentTime;
    }
    confirmPowerLongPressActive = false;
  }

  applyStateChange(nextState, currentTime);

  if (pressedEvents & (1 << BTN_POWER)) {
    powerButtonPressStart = confirmPowerPressStart;
  }

  if (emitConfirmClick) {
    pressedEvents |= (1 << BTN_CONFIRM);
    releasedEvents |= (1 << BTN_CONFIRM);
  }
}

void InputManager::updateDigitalTwoButton(const unsigned long currentTime) {
  const bool up = isDigitalPressed(BoardConfig::ACTIVE.input.up);
  const bool down = isDigitalPressed(BoardConfig::ACTIVE.input.down);
  const uint8_t physical = static_cast<uint8_t>((up ? 1u : 0u) | (down ? 2u : 0u));
  uint8_t auxiliaryState = serviceTouch();
  if (s_buttonHook) auxiliaryState |= s_buttonHook();
#if FREEINK_DEVICE_PAPERMONO
  // The power button reaches only the PM1 PMIC; clicks surface here as a
  // one-tick BTN_POWER pulse in the STATE, so applyStateChange() emits the
  // press this update and the release on the next. Never write the event
  // masks directly — applyStateChange() assigns them from the state diff,
  // clobbering direct writes the same tick.
  if (freeink::papermono::pollPowerButtonClicked(currentTime)) {
    auxiliaryState |= static_cast<uint8_t>(1u << BTN_POWER);
  }
#endif

  if (physical != twoButtonPhysicalState) {
    const uint8_t releasedPhysical = twoButtonPhysicalState;
    const bool emitShort = physical == 0 && !twoButtonLongPressActive;

    applyStateChange(auxiliaryState, currentTime);
    if (emitShort && (releasedPhysical == 1 || releasedPhysical == 2)) {
      const uint8_t logical = releasedPhysical == 1 ? BTN_UP : BTN_DOWN;
      pressedEvents |= static_cast<uint8_t>(1u << logical);
      releasedEvents |= static_cast<uint8_t>(1u << logical);
      buttonPressStart = twoButtonPressStart;
      buttonPressFinish = currentTime;
    }

    twoButtonPhysicalState = physical;
    twoButtonPressStart = currentTime;
    twoButtonLongPressActive = false;
    return;
  }

  if (physical == 0) {
    applyStateChange(auxiliaryState, currentTime);
    return;
  }

  uint8_t nextState = auxiliaryState;
  if (currentTime - twoButtonPressStart >= TWO_BUTTON_HOLD_MS) {
    twoButtonLongPressActive = true;
    const uint8_t logical = physical == 1 ? BTN_BACK : physical == 2 ? BTN_CONFIRM : BTN_POWER;
    nextState |= static_cast<uint8_t>(1u << logical);
  }
  applyStateChange(nextState, currentTime);
  if (pressedEvents & (1u << BTN_POWER)) powerButtonPressStart = twoButtonPressStart;
}

void InputManager::update() {
  const unsigned long currentTime = millis();

  pressedEvents = 0;
  releasedEvents = 0;
  touchPressedEvent = false;  // one-shot touch coord events, cleared each update()
  touchReleasedEvent = false;
  touchLongPressEvent = false;
  multiTouchSwipeEvent = false;
  multiTouchRotationEvent = false;
  touchHomeKeyEvent = false;
  touchHomeKeyTapEvent = false;
  touchHomeKeyLongEvent = false;

  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalConfirmBackHold) {
    updateConfirmBackHold(currentTime);
    return;
  }
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    updateConfirmPowerHold(currentTime);
    return;
  }
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalTwoButton) {
    updateDigitalTwoButton(currentTime);
    return;
  }

  const uint8_t state = getState();

  // Debounce
  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  if ((currentTime - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (state != currentState) {
      applyStateChange(state, currentTime);
    }
  }
}

bool InputManager::isPressed(const uint8_t buttonIndex) const { return currentState & (1 << buttonIndex); }

bool InputManager::wasPressed(const uint8_t buttonIndex) const { return pressedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyPressed() const { return pressedEvents > 0; }

bool InputManager::wasReleased(const uint8_t buttonIndex) const { return releasedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyReleased() const { return releasedEvents > 0; }

unsigned long InputManager::getHeldTime() const {
  // Still hold a button
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }

  return buttonPressFinish - buttonPressStart;
}

unsigned long InputManager::getPowerButtonHeldTime() const {
  if (isPressed(BTN_POWER)) {
    return millis() - powerButtonPressStart;
  }

  return powerButtonPressFinish - powerButtonPressStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::s_sharedConfirmPowerShortPressEmitsPower = false;

bool InputManager::isPowerButtonPressed() const { return isPressed(BTN_POWER); }

// ============================================================================
// Capacitive touch
//
// The public touch API is always available. Compiled only when
// FREEINK_CAP_TOUCH is set; the backend dispatches on
// BoardConfig::ACTIVE.touch.controller:
//   * CHSC6x (Murphy M3) — IRQ-driven, hand-rolled 16-byte frame decode.
//   * GT911  (LilyGo)    — polled status/point registers over I2C.
//   * FT5x06 (Paper Mono FT6336) — active-low IRQ + 0x02 point frame.
// Coordinates are delivered raw-panel-oriented; the app owns rotation.
// ============================================================================

bool InputManager::hasTouch() const {
#if FREEINK_CAP_TOUCH
  return touchDataEnabled;
#else
  return false;  // touch code not compiled in (FREEINK_CAP_TOUCH=0)
#endif
}

InputManager::TouchPoint InputManager::getTouchPoint() const { return touchPoint; }

bool InputManager::supportsMultiTouch() const {
#if FREEINK_CAP_TOUCH
  return touchDataEnabled && BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::Gt911;
#else
  return false;
#endif
}

InputManager::TouchSnapshot InputManager::getTouchSnapshot() const {
#if FREEINK_CAP_TOUCH
  return touchSnapshot;
#else
  return {};
#endif
}

bool InputManager::isTouchPressed() const { return touchPressed; }
bool InputManager::wasTouchPressed() const { return touchPressedEvent && !touchMultiContactSequence; }
bool InputManager::wasTouchReleased() const {
  // A multi-touch gesture must still provide the raw release edge so UI code
  // can drop pressed-state feedback, even though its tap/drag classifiers are
  // suppressed below.
  return touchReleasedEvent && (!touchSuppressed || touchMultiContactSequence);
}

void InputManager::normalizeTouchPoint(const uint16_t x, const uint16_t y, float& nx, float& ny) const {
  const auto& t = BoardConfig::ACTIVE.touch;
  const uint16_t w = (t.rawMaxX > t.rawMinX) ? static_cast<uint16_t>(t.rawMaxX - t.rawMinX) : 1;
  const uint16_t h = (t.rawMaxY > t.rawMinY) ? static_cast<uint16_t>(t.rawMaxY - t.rawMinY) : 1;
  const auto clamp01 = [](const float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); };
  nx = clamp01(static_cast<float>(x) / w);
  ny = clamp01(static_cast<float>(y) / h);
}

bool InputManager::wasTouchTap(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  if (!touchReleasedEvent || touchSuppressed || touchMultiContactSequence) return false;
  // Hold/long-press detection uses the tighter 28 px stationary slop, but a
  // released tap remains valid until motion reaches the 60 px swipe threshold.
  // Using the stationary threshold here created a 29..59 px dead band where a
  // normal finger roll was neither a tap nor a swipe.
  if (touchMovedBeyondTapReleaseSlop) return false;
  // Tap position = the FIRST contact sample (touch-down), not the last: the
  // reported centroid drifts 10-20px as a finger rolls off during lift, which
  // made small targets (steppers) feel unreliable with release-point routing.
  // A tap routes to where the user touched, not where the finger let go.
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nx, ny);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

bool InputManager::wasTouchPressedAt(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  // Press-edge analogue of wasTouchTap: true on the frame a touch begins,
  // writing the touch-down position normalized 0..1 in the panel's native
  // frame. Lets the app highlight what's under the finger on touch-down (before
  // release).
  if (!touchPressedEvent || touchMultiContactSequence) return false;
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nx, ny);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

bool InputManager::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
#if FREEINK_CAP_TOUCH
  if (!touchPressed || touchMovedBeyondTapSlop || touchSuppressed || touchMultiContactSequence) return false;
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nx, ny);
  heldMs = millis() - touchDownPoint.timestamp;
  return true;
#else
  (void)nx;
  (void)ny;
  (void)heldMs;
  return false;
#endif
}

bool InputManager::isTouchHeldAt(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  // Live drag tracking: the latest contact sample (touchUpPoint is refreshed on
  // every sample while pressed), with no tap-slop gate.
  if (!touchPressed || touchSuppressed || touchMultiContactSequence) return false;
  normalizeTouchPoint(touchUpPoint.x, touchUpPoint.y, nx, ny);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

unsigned long InputManager::lastTouchHeldMs() const {
#if FREEINK_CAP_TOUCH
  return lastTouchHeldDurationMs;
#else
  return 0;
#endif
}

bool InputManager::wasTouchActivity() const {
#if FREEINK_CAP_TOUCH
  return touchPressedEvent || touchReleasedEvent;
#else
  return false;
#endif
}

bool InputManager::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
#if FREEINK_CAP_TOUCH
  if (!touchReleasedEvent || touchSuppressed || touchMultiContactSequence) return false;
  // A flick: travelled past a distance threshold within a time window. Distance
  // is measured in native px; the dominant axis is left to the app (after
  // mapping to its logical frame).
  if (lastTouchHeldDurationMs > TOUCH_SWIPE_MAX_MS) return false;
  const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
  const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
  const int adx = absInt(dx);
  const int ady = absInt(dy);
  if (adx < TOUCH_SWIPE_MIN_PX && ady < TOUCH_SWIPE_MIN_PX) return false;
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nxStart, nyStart);
  normalizeTouchPoint(touchUpPoint.x, touchUpPoint.y, nxEnd, nyEnd);
  return true;
#else
  (void)nxStart;
  (void)nyStart;
  (void)nxEnd;
  (void)nyEnd;
  return false;
#endif
}

bool InputManager::wasMultiTouchSwipe(uint8_t& contactCount, float& nxStart, float& nyStart, float& nxEnd, float& nyEnd,
                                      unsigned long& durationMs) const {
#if FREEINK_CAP_TOUCH
  if (!multiTouchSwipeEvent || touchSuppressed) return false;
  contactCount = multiTouchSwipeContactCount;
  normalizeTouchPoint(multiTouchSwipeStartX, multiTouchSwipeStartY, nxStart, nyStart);
  normalizeTouchPoint(multiTouchSwipeEndX, multiTouchSwipeEndY, nxEnd, nyEnd);
  durationMs = multiTouchSwipeDurationMs;
  return true;
#else
  (void)contactCount;
  (void)nxStart;
  (void)nyStart;
  (void)nxEnd;
  (void)nyEnd;
  (void)durationMs;
  return false;
#endif
}

bool InputManager::wasMultiTouchRotation(float& degrees, float& nxCenter, float& nyCenter,
                                         unsigned long& durationMs) const {
#if FREEINK_CAP_TOUCH
  if (!multiTouchRotationEvent || touchSuppressed) return false;
  degrees = multiTouchRotationDegrees;
  normalizeTouchPoint(multiTouchRotationCenterX, multiTouchRotationCenterY, nxCenter, nyCenter);
  durationMs = multiTouchRotationDurationMs;
  return true;
#else
  (void)degrees;
  (void)nxCenter;
  (void)nyCenter;
  (void)durationMs;
  return false;
#endif
}

bool InputManager::wasTouchLongPress(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  if (!touchLongPressEvent || touchMultiContactSequence) return false;
  // Long-press routes to the touch-down point, same rationale as wasTouchTap.
  normalizeTouchPoint(touchDownPoint.x, touchDownPoint.y, nx, ny);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

void InputManager::suppressTouchContact() {
#if FREEINK_CAP_TOUCH
  // Only meaningful mid-contact (or on its release-edge frame); the latch
  // self-clears in serviceTouch() once the contact is fully over.
  if (touchPressed || touchReleasedEvent) touchSuppressed = true;
  cancelMultiTouchGesture();
  if (_asyncMultiTouchSwipeQueue) xQueueReset(_asyncMultiTouchSwipeQueue);
  if (_asyncMultiTouchRotationQueue) xQueueReset(_asyncMultiTouchRotationQueue);
#endif
}

void InputManager::startMultiTouchGesture(const TouchSnapshot& snapshot, const unsigned long now) {
  if (snapshot.count < 2 || snapshot.count > MAX_TOUCH_CONTACTS || snapshot.reportedCount != snapshot.count) {
    blockMultiTouchGesture();
    return;
  }

  if (snapshot.idsStable) {
    for (uint8_t i = 0; i < snapshot.count; ++i) {
      for (uint8_t j = i + 1; j < snapshot.count; ++j) {
        if (snapshot.points[i].id == snapshot.points[j].id) {
          blockMultiTouchGesture();
          return;
        }
      }
    }
  }

  trackedTouchContactCount = snapshot.count;
  multiTouchRotationEligible = trackedTouchContactCount == 2;
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    multiTouchContacts[i] = {snapshot.points[i].id, snapshot.points[i].point, snapshot.points[i].point};
    multiTouchContacts[i].start.timestamp = now;
  }
  for (uint8_t i = trackedTouchContactCount; i < MAX_TOUCH_CONTACTS; ++i) multiTouchContacts[i] = {};

  multiTouchGestureState = MultiTouchGestureState::Tracking;
  touchMultiContactSequence = true;
  // Any multi-contact sequence invalidates every single-contact classifier.
  touchMovedBeyondTapSlop = true;
  touchMovedBeyondTapReleaseSlop = true;
  touchLongPressEvent = false;
  touchLongPressFired = true;
}

void InputManager::blockMultiTouchGesture() {
  multiTouchGestureState = MultiTouchGestureState::Blocked;
  touchMultiContactSequence = true;
  touchMovedBeyondTapSlop = true;
  touchMovedBeyondTapReleaseSlop = true;
  touchLongPressEvent = false;
  touchLongPressFired = true;
}

void InputManager::resetMultiTouchGesture() {
  multiTouchGestureState = MultiTouchGestureState::Idle;
  trackedTouchContactCount = 0;
  multiTouchRotationEligible = false;
  touchMultiContactSequence = false;
  for (auto& contact : multiTouchContacts) contact = {};
}

void InputManager::cancelMultiTouchGesture() {
  multiTouchSwipeEvent = false;
  multiTouchRotationEvent = false;
  multiTouchGestureState =
      (touchPressed || touchReleasedEvent) ? MultiTouchGestureState::Blocked : MultiTouchGestureState::Idle;
}

bool InputManager::findContactAssignment(const TouchSnapshot& snapshot, const uint8_t trackedCount,
                                         uint8_t assignment[MAX_TOUCH_CONTACTS]) const {
  if (trackedCount == 0 || trackedCount > snapshot.count || snapshot.count > MAX_TOUCH_CONTACTS) return false;

  if (snapshot.idsStable) {
    uint8_t used = 0;
    for (uint8_t tracked = 0; tracked < trackedCount; ++tracked) {
      bool found = false;
      for (uint8_t current = 0; current < snapshot.count; ++current) {
        if ((used & (1u << current)) || snapshot.points[current].id != multiTouchContacts[tracked].id) continue;
        assignment[tracked] = current;
        used |= 1u << current;
        found = true;
        break;
      }
      if (!found) return false;
    }
    return true;
  }

  // Coordinate-only GT911 variants have no persistent contact identity. Find
  // the unique lowest-movement injective assignment. With at most four points
  // the exhaustive search is bounded at 4^4 candidates and allocates nothing.
  const auto squaredDistance = [](const TouchPoint& a, const TouchPoint& b) {
    const int64_t dx = static_cast<int64_t>(a.x) - static_cast<int64_t>(b.x);
    const int64_t dy = static_cast<int64_t>(a.y) - static_cast<int64_t>(b.y);
    return dx * dx + dy * dy;
  };

  uint16_t candidateCount = 1;
  for (uint8_t i = 0; i < trackedCount; ++i) candidateCount *= snapshot.count;
  int64_t bestDistance = INT64_MAX;
  int64_t secondDistance = INT64_MAX;
  uint8_t bestAssignment[MAX_TOUCH_CONTACTS] = {};
  for (uint16_t code = 0; code < candidateCount; ++code) {
    uint16_t remaining = code;
    uint8_t used = 0;
    uint8_t candidate[MAX_TOUCH_CONTACTS] = {};
    int64_t distance = 0;
    bool valid = true;
    for (uint8_t tracked = 0; tracked < trackedCount; ++tracked) {
      const uint8_t current = remaining % snapshot.count;
      remaining /= snapshot.count;
      if (used & (1u << current)) {
        valid = false;
        break;
      }
      used |= 1u << current;
      candidate[tracked] = current;
      distance += squaredDistance(multiTouchContacts[tracked].last, snapshot.points[current].point);
    }
    if (!valid) continue;
    if (distance < bestDistance) {
      secondDistance = bestDistance;
      bestDistance = distance;
      std::copy(candidate, candidate + trackedCount, bestAssignment);
    } else if (distance < secondDistance) {
      secondDistance = distance;
    }
  }

  if (bestDistance == INT64_MAX) return false;
  if (secondDistance != INT64_MAX && secondDistance - bestDistance <= TOUCH_CONTACT_ASSIGNMENT_AMBIGUITY_PX_SQ) {
    return false;
  }
  std::copy(bestAssignment, bestAssignment + trackedCount, assignment);
  return true;
}

bool InputManager::matchMultiTouchSnapshot(const TouchSnapshot& snapshot) {
  if (snapshot.count != trackedTouchContactCount) return false;
  uint8_t assignment[MAX_TOUCH_CONTACTS] = {};
  if (!findContactAssignment(snapshot, trackedTouchContactCount, assignment)) return false;
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    multiTouchContacts[i].last = snapshot.points[assignment[i]].point;
  }
  return true;
}

bool InputManager::expandMultiTouchGesture(const TouchSnapshot& snapshot, const unsigned long now) {
  if (snapshot.count <= trackedTouchContactCount || snapshot.count > MAX_TOUCH_CONTACTS) return false;

  uint8_t assignment[MAX_TOUCH_CONTACTS] = {};
  if (!findContactAssignment(snapshot, trackedTouchContactCount, assignment)) return false;
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    const TouchPoint& current = snapshot.points[assignment[i]].point;
    if (absInt(static_cast<int>(current.x) - multiTouchContacts[i].start.x) > TOUCH_TAP_SLOP_PX ||
        absInt(static_cast<int>(current.y) - multiTouchContacts[i].start.y) > TOUCH_TAP_SLOP_PX) {
      return false;
    }
  }

  // Fingers commonly land a few frames apart. While the existing contacts are
  // still stationary, adopt the larger cardinality and start the translation
  // from this coherent frame. A contact joining after motion begins is rejected.
  startMultiTouchGesture(snapshot, now);
  return multiTouchGestureState == MultiTouchGestureState::Tracking;
}

bool InputManager::isTrackedContact(const MultiTouchPoint& point) const {
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    if (point.id == multiTouchContacts[i].id) return true;
  }
  return false;
}

bool InputManager::hasStableTranslationGeometry() const {
  for (uint8_t first = 0; first < trackedTouchContactCount; ++first) {
    for (uint8_t second = first + 1; second < trackedTouchContactCount; ++second) {
      const int startSeparationX =
          static_cast<int>(multiTouchContacts[second].start.x) - multiTouchContacts[first].start.x;
      const int startSeparationY =
          static_cast<int>(multiTouchContacts[second].start.y) - multiTouchContacts[first].start.y;
      const int endSeparationX = static_cast<int>(multiTouchContacts[second].last.x) - multiTouchContacts[first].last.x;
      const int endSeparationY = static_cast<int>(multiTouchContacts[second].last.y) - multiTouchContacts[first].last.y;
      if (absInt(endSeparationX - startSeparationX) > TOUCH_MULTI_CONTACT_SEPARATION_SLOP_PX ||
          absInt(endSeparationY - startSeparationY) > TOUCH_MULTI_CONTACT_SEPARATION_SLOP_PX) {
        return false;
      }
    }
  }
  return true;
}

bool InputManager::hasEligibleRotationScale() const {
  if (trackedTouchContactCount != 2) return false;
  const auto toGesturePoint = [](const TouchPoint& point) {
    return freeink::input_detail::GesturePoint{point.x, point.y};
  };
  return freeink::input_detail::hasRotationScale(
      toGesturePoint(multiTouchContacts[0].start), toGesturePoint(multiTouchContacts[1].start),
      toGesturePoint(multiTouchContacts[0].last), toGesturePoint(multiTouchContacts[1].last));
}

bool InputManager::isMultiTouchTranslation(const unsigned long now) const {
  if (trackedTouchContactCount < 2 || now - multiTouchContacts[0].start.timestamp > TOUCH_MULTI_SWIPE_MAX_MS ||
      !hasStableTranslationGeometry()) {
    return false;
  }

  int startCenterX = 0;
  int startCenterY = 0;
  int endCenterX = 0;
  int endCenterY = 0;
  for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
    startCenterX += multiTouchContacts[i].start.x;
    startCenterY += multiTouchContacts[i].start.y;
    endCenterX += multiTouchContacts[i].last.x;
    endCenterY += multiTouchContacts[i].last.y;
  }
  startCenterX /= trackedTouchContactCount;
  startCenterY /= trackedTouchContactCount;
  endCenterX /= trackedTouchContactCount;
  endCenterY /= trackedTouchContactCount;
  const int centerDx = endCenterX - startCenterX;
  const int centerDy = endCenterY - startCenterY;
  const int centerAbsX = absInt(centerDx);
  const int centerAbsY = absInt(centerDy);

  if (centerAbsX >= TOUCH_SWIPE_MIN_PX && centerAbsX * 2 >= centerAbsY * 3) {
    for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
      const int dx = static_cast<int>(multiTouchContacts[i].last.x) - multiTouchContacts[i].start.x;
      if (absInt(dx) < TOUCH_SWIPE_MIN_PX || (dx > 0) != (centerDx > 0)) return false;
    }
    return true;
  }
  if (centerAbsY >= TOUCH_SWIPE_MIN_PX && centerAbsY * 2 >= centerAbsX * 3) {
    for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
      const int dy = static_cast<int>(multiTouchContacts[i].last.y) - multiTouchContacts[i].start.y;
      if (absInt(dy) < TOUCH_SWIPE_MIN_PX || (dy > 0) != (centerDy > 0)) return false;
    }
    return true;
  }
  return false;
}

bool InputManager::classifyMultiTouchRotation(const unsigned long now) {
  if (!multiTouchRotationEligible || trackedTouchContactCount != 2 ||
      now - multiTouchContacts[0].start.timestamp > TOUCH_MULTI_SWIPE_MAX_MS) {
    return false;
  }

  const auto toGesturePoint = [](const TouchPoint& point) {
    return freeink::input_detail::GesturePoint{point.x, point.y};
  };
  freeink::input_detail::RotationResult result;
  if (!freeink::input_detail::classifyRotation(
          toGesturePoint(multiTouchContacts[0].start), toGesturePoint(multiTouchContacts[1].start),
          toGesturePoint(multiTouchContacts[0].last), toGesturePoint(multiTouchContacts[1].last), result)) {
    return false;
  }

  multiTouchRotationDegrees = result.degrees;
  multiTouchRotationCenterX = result.centerX;
  multiTouchRotationCenterY = result.centerY;
  multiTouchRotationDurationMs = static_cast<uint16_t>(now - multiTouchContacts[0].start.timestamp);
  multiTouchRotationEvent = true;
  return true;
}

void InputManager::finishMultiTouchGesture(const unsigned long now) {
  if (classifyMultiTouchRotation(now)) {
    multiTouchGestureState = MultiTouchGestureState::Blocked;
    return;
  }
  if (isMultiTouchTranslation(now)) {
    uint32_t startX = 0;
    uint32_t startY = 0;
    uint32_t endX = 0;
    uint32_t endY = 0;
    for (uint8_t i = 0; i < trackedTouchContactCount; ++i) {
      startX += multiTouchContacts[i].start.x;
      startY += multiTouchContacts[i].start.y;
      endX += multiTouchContacts[i].last.x;
      endY += multiTouchContacts[i].last.y;
    }
    multiTouchSwipeContactCount = trackedTouchContactCount;
    multiTouchSwipeStartX = static_cast<uint16_t>(startX / trackedTouchContactCount);
    multiTouchSwipeStartY = static_cast<uint16_t>(startY / trackedTouchContactCount);
    multiTouchSwipeEndX = static_cast<uint16_t>(endX / trackedTouchContactCount);
    multiTouchSwipeEndY = static_cast<uint16_t>(endY / trackedTouchContactCount);
    multiTouchSwipeDurationMs = static_cast<uint16_t>(now - multiTouchContacts[0].start.timestamp);
    multiTouchSwipeEvent = true;
  }
  multiTouchGestureState = MultiTouchGestureState::Blocked;
}

void InputManager::updateMultiTouchGesture(const TouchSnapshot& snapshot, const unsigned long now) {
  if (snapshot.reportedCount > MAX_TOUCH_CONTACTS || snapshot.count != snapshot.reportedCount) {
    blockMultiTouchGesture();
    return;
  }
  if (snapshot.count == 0) {
    if (multiTouchGestureState == MultiTouchGestureState::Tracking) finishMultiTouchGesture(now);
    return;
  }
  if (multiTouchGestureState == MultiTouchGestureState::Blocked) return;
  if (multiTouchGestureState == MultiTouchGestureState::Idle) {
    if (snapshot.count < 2) return;
    // A finger that has already dragged or long-pressed owns this contact;
    // joining more fingers must not retroactively convert it into a swipe.
    if (touchPressed && (touchMovedBeyondTapSlop || touchLongPressFired)) {
      blockMultiTouchGesture();
      return;
    }
    startMultiTouchGesture(snapshot, now);
    return;
  }
  if (snapshot.count < trackedTouchContactCount) {
    // Stable IDs let us reject a replacement contact arriving in the same
    // frame that one of the original contacts leaves.
    if (snapshot.idsStable) {
      for (uint8_t i = 0; i < snapshot.count; ++i) {
        if (!isTrackedContact(snapshot.points[i])) {
          blockMultiTouchGesture();
          return;
        }
        for (uint8_t j = i + 1; j < snapshot.count; ++j) {
          if (snapshot.points[i].id == snapshot.points[j].id) {
            blockMultiTouchGesture();
            return;
          }
        }
      }
    }
    finishMultiTouchGesture(now);  // first tracked contact left
    return;
  }
  if (snapshot.count > trackedTouchContactCount) {
    if (!expandMultiTouchGesture(snapshot, now)) blockMultiTouchGesture();
    return;
  }
  if (!matchMultiTouchSnapshot(snapshot)) {
    blockMultiTouchGesture();
    return;
  }
  if (multiTouchRotationEligible && !hasEligibleRotationScale()) multiTouchRotationEligible = false;
}

bool InputManager::wasHomeKeyPressed() const { return touchHomeKeyEvent; }

bool InputManager::wasHomeKeyTapped() const { return touchHomeKeyTapEvent; }

bool InputManager::wasHomeKeyLongPressed() const { return touchHomeKeyLongEvent; }

void InputManager::beginTouch() {
#if FREEINK_CAP_TOUCH
  const auto& t = BoardConfig::ACTIVE.touch;
  if (t.controller == BoardConfig::TouchController::None) {
    return;
  }
  if (t.controller == BoardConfig::TouchController::Gt911) {
    beginGt911();
    return;
  }
  if (t.controller == BoardConfig::TouchController::Ft5x06) {
    beginFt5x06();
    return;
  }
  if (t.controller == BoardConfig::TouchController::Ft6336u) {
    beginFt6336u();
    return;
  }
  // CHSC6x: I2C bus only. The IRQ is left unconfigured — it's a brief pulse on
  // this controller, so detection polls I2C and gates on the frame's touch bit
  // instead (see decodeChsc6xFrame / updateTouchFromIrq).
  if (t.sda >= 0 && t.scl >= 0 && t.i2cAddress != 0) {
    Wire.begin(t.sda, t.scl, 100000);
    Wire.setTimeOut(4);
    touchDataEnabled = true;
  }
#endif
}

uint8_t InputManager::serviceTouch() {
#if FREEINK_CAP_TOUCH
  if (!touchDataEnabled) {
    return 0;
  }
  const unsigned long now = millis();
  const auto& t = BoardConfig::ACTIVE.touch;

  // Contact bookkeeping shared by all backends. Runs BEFORE the poll so the
  // suppression latch releases on the first fully-idle frame (contact over,
  // release edge consumed) and a new contact beginning in this same call is
  // delivered normally.
  if (!touchPressed && !touchReleasedEvent) {
    touchSuppressed = false;
    touchLongPressFired = false;
    resetMultiTouchGesture();
  }

  if (t.controller == BoardConfig::TouchController::Gt911) {
    pollGt911(now);
  } else if (t.controller == BoardConfig::TouchController::Ft5x06) {
    pollFt5x06(now);
  } else if (t.controller == BoardConfig::TouchController::Ft6336u) {
    pollFt6336u(now);
  } else {
    updateTouchFromIrq(now, 0);  // detection polls I2C; the IRQ is unused now
    // Synthesized confirm tracks an actually-detected press, not the IRQ line.
    if (touchPressedEvent) touchIrqPulseUntil = now + TOUCH_IRQ_PULSE_MS;
  }

  // Long-press classification, beside the tap/swipe machinery it shares state
  // with. Fires once per contact, while the finger is still down.
  if (touchPressed && !touchMultiContactSequence && !touchMovedBeyondTapSlop && !touchLongPressFired &&
      !touchSuppressed && now - touchDownPoint.timestamp >= TOUCH_LONG_PRESS_MS) {
    touchLongPressFired = true;
    touchLongPressEvent = true;
  }

  return (t.synthesizeConfirm && now < touchIrqPulseUntil) ? (1 << BTN_CONFIRM) : 0;
#else
  return 0;
#endif
}

#if FREEINK_CAP_TOUCH

void InputManager::updateTouchFromIrq(const unsigned long now, const int irqRaw) {
  // Poll the controller over I2C on a fixed cadence, independent of the IRQ.
  // The CHSC6x IRQ is a brief (~24ms) pulse at touch-down, not a level held for
  // the contact, so edge/level-gated reads missed quick taps. readChsc6xPoint
  // only returns true for a real touch (data[3] touch bit), so polling can't
  // latch the idle phantom frame. A valid read sets the press and refreshes the
  // release deadline; once reads stop coming, the touch releases after a short
  // hold-over.
  (void)irqRaw;
  if (now >= touchReadAt) {
    touchReadAt = now + TOUCH_SAMPLE_DELAY_MS;
    TouchPoint point = {false, 0, 0, 0};
    if (readChsc6xPoint(point)) {
      touchPoint = point;
      if (!touchPressed) {
        touchPressed = true;
        touchPressedEvent = true;
        touchDownPoint = point;  // first contact sample, used for tap routing
        touchUpPoint = point;
        touchMovedBeyondTapSlop = false;
        touchMovedBeyondTapReleaseSlop = false;
      } else {
        touchUpPoint = point;
        const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
        const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
        if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
          touchMovedBeyondTapSlop = true;
        }
        if (absInt(dx) > TOUCH_TAP_RELEASE_SLOP_PX || absInt(dy) > TOUCH_TAP_RELEASE_SLOP_PX) {
          touchMovedBeyondTapReleaseSlop = true;
        }
      }
      touchReleaseAt = now + TOUCH_IRQ_PULSE_MS;
    }
  }

  if (touchPressed && now >= touchReleaseAt) {
    touchPressed = false;
    touchReleasedEvent = true;
    lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
  }
}

bool InputManager::readChsc6xPoint(TouchPoint& point) {
  const uint8_t addr = BoardConfig::ACTIVE.touch.i2cAddress;
  Wire.beginTransmission(addr);
  Wire.write(TOUCH_READ_COMMAND);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t data[TOUCH_FRAME_SIZE] = {};
  const uint8_t received = Wire.requestFrom(addr, TOUCH_FRAME_SIZE, static_cast<uint8_t>(true));
  if (received != TOUCH_FRAME_SIZE) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < TOUCH_FRAME_SIZE; ++i) {
    data[i] = Wire.read();
  }
  return decodeChsc6xFrame(data, TOUCH_FRAME_SIZE, point);
}

bool InputManager::decodeChsc6xFrame(const uint8_t* data, const size_t len, TouchPoint& point) const {
  if (len < 7) {
    return false;
  }
  // data[3] bit 7 is the touch-present flag: 0x80 while a finger is down, 0x00
  // when idle. The controller keeps returning a stale coordinate frame between
  // touches, so without this gate every read looks like a phantom touch (which
  // is why polling reported a fixed point and IRQ-gated reads were needed to
  // dodge it). Release transitions briefly show 0x40/0xff — both fail this test
  // or the coordinate sanity check below.
  if ((data[3] & 0x80) == 0) {
    return false;
  }
  const uint16_t rawX = data[4];                                          // X: one byte
  const uint16_t rawY = (static_cast<uint16_t>(data[5]) << 8) | data[6];  // Y: 16-bit big-endian
  if ((rawX == 0 && rawY == 0) || (rawX == 0xff && rawY == 0xffff)) {
    return false;
  }
  const auto& t = BoardConfig::ACTIVE.touch;
  point.valid = true;
  // Panel-native coordinates (the calibrated raw range, in the touch panel's
  // own orientation); the app maps to its display/logical frame. See the touch
  // note in the README.
  point.x = mapTouchAxis(rawX, t.rawMinX, t.rawMaxX, t.rawMaxX - t.rawMinX);
  point.y = mapTouchAxis(rawY, t.rawMinY, t.rawMaxY, t.rawMaxY - t.rawMinY);
  point.timestamp = millis();
  return true;
}

uint16_t InputManager::mapTouchAxis(uint16_t raw, const uint16_t rawMin, const uint16_t rawMax,
                                    const uint16_t outMax) const {
  if (raw <= rawMin) return 0;
  if (raw >= rawMax) return outMax;
  return static_cast<uint32_t>(raw - rawMin) * outMax / (rawMax - rawMin);
}

// --- FT5x06 / FT6336 (M5Stack Paper Mono) ----------------------------------

bool InputManager::ft5x06WriteReg(const uint8_t reg, const uint8_t value) {
  const uint8_t addr = BoardConfig::ACTIVE.touch.i2cAddress;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool InputManager::ft5x06ReadReg(const uint8_t reg, uint8_t* buf, const uint8_t len) {
  const uint8_t addr = BoardConfig::ACTIVE.touch.i2cAddress;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  const uint8_t got = Wire.requestFrom(addr, len, static_cast<uint8_t>(true));
  if (got != len) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
  return true;
}

void InputManager::beginFt5x06() {
  const auto& t = BoardConfig::ACTIVE.touch;
  if (t.sda < 0 || t.scl < 0 || t.i2cAddress == 0) return;

#if FREEINK_DEVICE_PAPERMONO
  // The FT6336's power rail and reset line live on the M5IOE1 expander, not
  // ESP GPIOs — raise/release them before the probe below.
  freeink::papermono::enableTouch();
#endif

  // The bus is shared with M5PM1/M5IOE1/RX8130, whose standing profile is
  // 100 kHz. FT6336 accepts that rate even though M5GFX uses 400 kHz for its
  // controller-specific transactions.
  Wire.begin(t.sda, t.scl, 100000);
  Wire.setTimeOut(10);
  if (t.irq >= 0) pinMode(t.irq, INPUT_PULLUP);

  // Match M5GFX Touch_FT5x06::_check_init(): enter working mode, read the
  // chip/firmware/vendor window, then select polling/level interrupt mode.
  // Retried over ~600 ms: the FT6336 needs up to ~300 ms after a hardware
  // reset before its I2C interface answers, and on boards where the rail/reset
  // bring-up happens right here (Paper Mono: enableTouch() above) a one-shot
  // probe races the controller's boot and leaves touch dead for the session.
  // Gate on the transactions succeeding, NOT on the ID contents: Paper Mono
  // units ACK and serve the whole 0xA3..0xA8 window as zeros, so a vendor-byte
  // check reads as "absent" on a perfectly working controller.
  uint8_t id[6] = {};
  bool wrMode = false, rdId = false, wrIrq = false;
  for (int attempt = 0; attempt < 12 && !rdId; ++attempt) {
    if (attempt) delay(50);
    wrMode = ft5x06WriteReg(0x00, 0x00);
    rdId = wrMode && ft5x06ReadReg(0xA3, id, sizeof(id));
    wrIrq = rdId && ft5x06WriteReg(0xA4, 0x00);
  }
  touchDataEnabled = wrMode && rdId && wrIrq;
#ifdef TOUCH_PROBE_DEBUG
#if FREEINK_DEVICE_PAPERMONO
  // Expander state alongside the probe result: OUT should show TP_EN (bit 12)
  // and TP_RST (bit 5) high, MODE should show the configured output mask
  // (0x39B4). All-zero probe ids + correct expander state = the FT6336 itself
  // isn't answering; wrong expander state = the rail/reset never asserted.
  uint16_t ioeMode = 0xFFFF, ioeOut = 0xFFFF;
  freeink::m5ioe1::readReg16(freeink::m5ioe1::REG_GPIO_MODE_L, &ioeMode);
  freeink::m5ioe1::readReg16(freeink::m5ioe1::REG_GPIO_OUT_L, &ioeOut);
  touchDebugPrintf("[touch] IOE1 addr=0x%02X mode=0x%04X out=0x%04X\n", freeink::m5ioe1::g_addr, ioeMode, ioeOut);
  // Full bus scan with the touch rail up: expected residents are 0x32 (RX8130
  // RTC), 0x4F/0x6F (IOE1), 0x68 (BMI270), 0x6E (PM1), 0x50 (NFC on Pro) —
  // whatever ELSE ACKs is the touch controller (FT6336 = 0x38; some unit
  // revisions may carry a CST820 = 0x15 instead).
  touchDebugPrintf("[touch] i2c scan:");
  for (uint8_t a = 0x08; a <= 0x77; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) touchDebugPrintf(" 0x%02X", a);
    delayMicroseconds(200);
  }
  touchDebugPrintf("\n");
#endif
  touchDebugPrintf(
      "[touch] FT5x06 probe: enabled=%d wrMode=%d rdId=%d wrIrq=%d "
      "cipher=0x%02X fw=0x%02X vendor=0x%02X irq=%d\n",
      touchDataEnabled, wrMode, rdId, wrIrq, id[0], id[3], id[5], t.irq);
#endif
}

void InputManager::pollFt5x06(const unsigned long now) {
  const auto& t = BoardConfig::ACTIVE.touch;
  if (now < touchReadAt) return;
  touchReadAt = now + TOUCH_SAMPLE_DELAY_MS;

  // The controller runs in interrupt-polling mode (G_MODE=0, set in begin),
  // where INT emits low PULSES at the report rate while a contact is held —
  // the line reads HIGH between pulses even with the finger down, so its
  // level must not be treated as a release (that splits one swipe into a
  // phantom tap plus a swipe). Idle fast-path gate only; while a contact is
  // live, the TD_STATUS zero-contact frame below is the release authority.
  const bool irqDown = t.irq < 0 || digitalRead(t.irq) == LOW;
  if (!irqDown && !touchPressed) {
    return;
  }

  // Register 0x02 is TD_STATUS followed by the first point's XH/XL/YH/YL.
  // One contact is enough for the app's tap/swipe/drag gesture model.
  uint8_t data[5] = {};
  if (!ft5x06ReadReg(0x02, data, sizeof(data))) {
    // Transient read failures happen on the shared PY32 bus; survive them.
    // But a controller that has stopped answering (rail glitch) must not
    // leave the contact latched — release once samples go stale.
    constexpr unsigned long STALE_RELEASE_MS = 100;
    if (touchPressed && now - touchPoint.timestamp > STALE_RELEASE_MS) {
      touchPressed = false;
      touchPoint.valid = false;
      touchReleasedEvent = true;
      lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
    }
    return;
  }
  if ((data[0] & 0x0F) == 0) {
    // FT6336 may keep INT low until TD_STATUS has been drained. Treat the
    // controller's zero-contact frame as authoritative; waiting only for the
    // GPIO to rise leaves touchPressed latched and drops every later tap.
    if (touchPressed) {
      touchPressed = false;
      touchPoint.valid = false;
      touchReleasedEvent = true;
      lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
#ifdef TOUCH_PROBE_DEBUG
      touchDebugPrintf("[touch] FT release via TD_STATUS=0 held=%lums\n", lastTouchHeldDurationMs);
#endif
    }
    return;
  }
  const uint16_t rawX = static_cast<uint16_t>((data[1] & 0x0F) << 8) | data[2];
  const uint16_t rawY = static_cast<uint16_t>((data[3] & 0x0F) << 8) | data[4];
  const uint16_t sx = t.swapXY ? rawY : rawX;
  const uint16_t sy = t.swapXY ? rawX : rawY;

  touchPoint.valid = true;
  touchPoint.x = mapTouchAxis(sx, t.rawMinX, t.rawMaxX, t.rawMaxX - t.rawMinX);
  touchPoint.y = mapTouchAxis(sy, t.rawMinY, t.rawMaxY, t.rawMaxY - t.rawMinY);
  if (t.flipX) {
    touchPoint.x = static_cast<uint16_t>((t.rawMaxX - t.rawMinX) - touchPoint.x);
  }
  if (t.flipY) {
    touchPoint.y = static_cast<uint16_t>((t.rawMaxY - t.rawMinY) - touchPoint.y);
  }
  touchPoint.timestamp = now;

  if (!touchPressed) {
    touchPressed = true;
    touchPressedEvent = true;
    touchDownPoint = touchPoint;
    touchUpPoint = touchPoint;
    touchMovedBeyondTapSlop = false;
    touchMovedBeyondTapReleaseSlop = false;
#ifdef TOUCH_PROBE_DEBUG
    touchDebugPrintf("[touch] FT press raw=(%u,%u) panel=(%u,%u)\n", rawX, rawY, touchPoint.x, touchPoint.y);
#endif
  } else {
    touchUpPoint = touchPoint;
    const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
    const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
    if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
      touchMovedBeyondTapSlop = true;
    }
    if (absInt(dx) > TOUCH_TAP_RELEASE_SLOP_PX || absInt(dy) > TOUCH_TAP_RELEASE_SLOP_PX) {
      touchMovedBeyondTapReleaseSlop = true;
    }
  }
}

// --- GT911 (LilyGo) ---------------------------------------------------------

void InputManager::beginGt911() {
  const auto& t = BoardConfig::ACTIVE.touch;

  // Power the touch rail first (boards that gate it, e.g. Sticky's TOUCH_EN on
  // GPIO42). Active-high + settle, before the reset dance and I2C probe;
  // without this the GT911 never ACKs and touch is reported absent. No-op when
  // unassigned. gpio_hold_dis first: the sleep path holds this pin LOW and the
  // hold survives the deep-sleep wake reset; the HIGH write is a no-op until it
  // is released.
  if (t.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(t.powerEnable));
    pinMode(t.powerEnable, OUTPUT);
    // ON level: HIGH for active-high enables (Sticky), LOW for active-low (X4
    // Pro GPIO2).
    digitalWrite(t.powerEnable, t.powerEnableActiveHigh ? HIGH : LOW);
    delay(50);
  }

  if (t.sda >= 0 && t.scl >= 0) {
    Wire.begin(t.sda, t.scl, 400000);
    Wire.setTimeOut(10);
  }

  // Release any pad hold left on RESET by the sleep path (TouchConfig::
  // holdResetInSleep parks the controller in reset on boards with no touch
  // rail). gpio_hold_en survives the deep-sleep wake reset, and a held pad
  // silently swallows the reset dance below — touch would come back dead after
  // the first wake, and only after a wake.
  if (t.reset >= 0) gpio_hold_dis(static_cast<gpio_num_t>(t.reset));

  auto resetWithIntLevel = [&](const uint8_t level) {
    if (t.reset < 0 || t.irq < 0) return;
    pinMode(t.irq, OUTPUT);
    pinMode(t.reset, OUTPUT);
    digitalWrite(t.reset, LOW);
    digitalWrite(t.irq, level);
    delay(10);
    digitalWrite(t.reset, HIGH);
    delay(10);
    digitalWrite(t.irq, level);
    delay(50);
    pinMode(t.irq, INPUT);
    delay(50);
  };

  auto probeCandidates = [&]() {
    const uint8_t candidates[2] = {t.i2cAddress, t.i2cAddressAlt};
    for (uint8_t a : candidates) {
      if (a == 0) continue;
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        gt911Addr = a;
        return true;
      }
    }
    return false;
  };

  // Reset + address-select dance: INT level as RST rises selects the address.
  // Boards differ in which strapped address survives their module wiring, so
  // try the primary-select level first, then the alternate level before
  // declaring the touch controller absent.
  gt911Addr = 0;
  resetWithIntLevel(LOW);
  if (!probeCandidates()) {
    resetWithIntLevel(HIGH);
    probeCandidates();
  }

  touchDataEnabled = (gt911Addr != 0);
#ifdef TOUCH_PROBE_DEBUG
  touchDebugPrintf(
      "[touch] GT911 probe: addr=0x%02X enabled=%d (sda=%d scl=%d "
      "cand=0x%02X/0x%02X)\n",
      gt911Addr, touchDataEnabled, t.sda, t.scl, t.i2cAddress, t.i2cAddressAlt);
#endif
}

bool InputManager::gt911ReadReg(const uint16_t reg, uint8_t* buf, const uint8_t len) {
  Wire.beginTransmission(gt911Addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  const uint8_t got = Wire.requestFrom(gt911Addr, len, static_cast<uint8_t>(true));
  if (got != len) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < len; ++i) {
    buf[i] = Wire.read();
  }
  return true;
}

void InputManager::gt911ClearStatus() {
  Wire.beginTransmission(gt911Addr);
  Wire.write(0x81);
  Wire.write(0x4E);
  Wire.write(static_cast<uint8_t>(0x00));
  Wire.endTransmission();
}

#if FREEINK_DEVICE_MURPHY_M4
// The M4 uses the ESP-IDF I2C master driver on controller 1. Keeping this bus
// separate from Arduino Wire mirrors the factory firmware and avoids conflicts
// with other SDK-managed buses.
namespace {
i2c_master_bus_handle_t gM4TouchBus = nullptr;
i2c_master_dev_handle_t gM4TouchDevice = nullptr;

static bool m4NativeWriteReg(const uint8_t reg, const uint8_t value) {
  if (gM4TouchDevice == nullptr) return false;
  const uint8_t payload[] = {reg, value};
  const esp_err_t err = i2c_master_transmit(gM4TouchDevice, payload, sizeof(payload), 20);
  if (err != ESP_OK) {
    esp_rom_printf("[touch] M4 write reg=%02X value=%02X failed: %s (%d)\r\n", reg, value, esp_err_to_name(err),
                   static_cast<int>(err));
  }
  return err == ESP_OK;
}

static bool m4NativeRead(const uint8_t reg, uint8_t* buf, const size_t len) {
  if (gM4TouchDevice == nullptr) return false;
  const esp_err_t err = i2c_master_transmit_receive(gM4TouchDevice, &reg, 1, buf, len, 20);
  if (err != ESP_OK) {
    esp_rom_printf("[touch] M4 read reg=%02X len=%u failed: %s (%d)\r\n", reg, static_cast<unsigned>(len),
                   esp_err_to_name(err), static_cast<int>(err));
  }
  return err == ESP_OK;
}

static bool m4NativeBegin(const int sda, const int scl, const uint8_t address) {
  if (gM4TouchDevice != nullptr) return true;
  i2c_master_bus_config_t busConfig = {};
  busConfig.i2c_port = I2C_NUM_1;
  busConfig.sda_io_num = static_cast<gpio_num_t>(sda);
  busConfig.scl_io_num = static_cast<gpio_num_t>(scl);
  busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
  busConfig.glitch_ignore_cnt = 7;
  busConfig.flags.enable_internal_pullup = 1;
  esp_err_t err = i2c_new_master_bus(&busConfig, &gM4TouchBus);
  if (err != ESP_OK) {
    esp_rom_printf("[touch] M4 i2c_new_master_bus failed: %s\r\n", esp_err_to_name(err));
    return false;
  }
  i2c_device_config_t deviceConfig = {};
  deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  deviceConfig.device_address = address;
  deviceConfig.scl_speed_hz = 100000;
  err = i2c_master_bus_add_device(gM4TouchBus, &deviceConfig, &gM4TouchDevice);
  if (err != ESP_OK) {
    esp_rom_printf("[touch] M4 i2c_master_bus_add_device failed: %s\r\n", esp_err_to_name(err));
    i2c_del_master_bus(gM4TouchBus);
    gM4TouchBus = nullptr;
    return false;
  }
  return true;
}

}  // namespace
#endif  // FREEINK_DEVICE_MURPHY_M4

void InputManager::beginFt6336u() {
  const auto& t = BoardConfig::ACTIVE.touch;

  if (t.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(t.powerEnable));
    pinMode(t.powerEnable, OUTPUT);
#if FREEINK_DEVICE_MURPHY_M4
    // Murphy Reader v1.2.16 powers the rail and allows 500 ms to settle.
    // This is runtime-only; no touch firmware/update commands are issued.
    digitalWrite(t.powerEnable, t.powerEnableActiveHigh ? HIGH : LOW);
    delay(500);
#else
    digitalWrite(t.powerEnable, t.powerEnableActiveHigh ? HIGH : LOW);
    delay(300);
#endif
  }

  if (t.sda >= 0 && t.scl >= 0) {
#if !FREEINK_DEVICE_MURPHY_M4
    Wire.begin(t.sda, t.scl, 400000);
    Wire.setTimeOut(10);
#endif
  }

#if FREEINK_DEVICE_MURPHY_M4
  if (t.sda >= 0 && t.scl >= 0) {
    // Exact Murphy Reader reset timing: RESET low 50 ms, high 100 ms.
    // GPIO7 is shared with the display reset line on this board.
    if (t.reset >= 0) {
      // Release a sleep-path hold first (TouchConfig::holdResetInSleep); it
      // survives the wake reset and would make the pulse below a no-op.
      gpio_hold_dis(static_cast<gpio_num_t>(t.reset));
      pinMode(t.reset, OUTPUT);
      digitalWrite(t.reset, LOW);
      delay(50);
      digitalWrite(t.reset, HIGH);
      delay(100);
    }
    if (t.irq >= 0) {
      pinMode(t.irq, INPUT_PULLUP);
    }
    const bool busOk = m4NativeBegin(t.sda, t.scl, t.i2cAddress);
    const bool initModeOk = busOk && m4NativeWriteReg(0x00, 0x00);
    const bool initThresholdOk = busOk && m4NativeWriteReg(0x80, 0x16);
    const bool initRateOk = busOk && m4NativeWriteReg(0x88, 0x04);
    esp_rom_printf("[touch] M4 OEM volatile init: mode=%d threshold=%d rate=%d\r\n", static_cast<int>(initModeOk),
                   static_cast<int>(initThresholdOk), static_cast<int>(initRateOk));
    // Murphy Reader reads the FT6336 status/point block at register 0x02.
    uint8_t probeFrame[11] = {};
    touchDataEnabled = busOk && m4NativeRead(0x02, probeFrame, sizeof(probeFrame));
    esp_rom_printf("[touch] M4 FT6336U ready=%d (SDA=%d SCL=%d address=0x%02X)\r\n", static_cast<int>(touchDataEnabled),
                   t.sda, t.scl, t.i2cAddress);
  }
#else
  // Probe with retries in case the controller is still stabilising.
  for (int attempt = 0; attempt < 3 && !touchDataEnabled; ++attempt) {
    if (attempt > 0) delay(100);
    Wire.beginTransmission(t.i2cAddress);
    touchDataEnabled = (Wire.endTransmission() == 0);
  }
#endif
}

// FT6336U register layout (read from reg 0x00, 7 bytes):
//   [0] device mode, [1] gesture ID, [2] touch point count
//   [3] P1 xH: event_flag[7:6], reserved[5:4], x[11:8]
//   [4] P1 xL: x[7:0]
//   [5] P1 yH: touch_id[7:4], y[11:8]
//   [6] P1 yL: y[7:0]
// event_flag: 0=press, 1=lift, 2=contact, 3=reserved. A count>0 with flag!=1 = touching.
void InputManager::pollFt6336u(const unsigned long now) {
  if (!touchDataEnabled) return;

#if FREEINK_DEVICE_MURPHY_M4
  // The FT6336U asserts INT low when a fresh sample is ready. Continue polling
  // while pressed so the release frame is consumed even if INT rises first.
  const int irq = BoardConfig::ACTIVE.touch.irq;
  if (irq >= 0 && digitalRead(irq) != LOW && !touchPressed) return;
#endif

  if (now < touchReadAt) return;
  touchReadAt = now + TOUCH_SAMPLE_DELAY_MS;

  uint8_t data[16] = {};
  bool gotData = false;
#if FREEINK_DEVICE_MURPHY_M4
  uint8_t nativeFrame[11] = {};
  gotData = m4NativeRead(0x02, nativeFrame, sizeof(nativeFrame));
  if (gotData) {
    // Convert register-0x02 layout to the canonical FT6336 layout consumed below.
    data[2] = nativeFrame[0];
    data[3] = nativeFrame[1];
    data[4] = nativeFrame[2];
    data[5] = nativeFrame[3];
    data[6] = nativeFrame[4];
  }
#else
  const uint8_t addr = BoardConfig::ACTIVE.touch.i2cAddress;
  Wire.beginTransmission(addr);
  Wire.write(static_cast<uint8_t>(0x00));
  if (Wire.endTransmission(false) == 0) {
    const uint8_t got = Wire.requestFrom(addr, static_cast<uint8_t>(7), static_cast<uint8_t>(true));
    if (got == 7) {
      for (uint8_t i = 0; i < 7; ++i) data[i] = Wire.read();
      gotData = true;
    } else {
      while (Wire.available()) Wire.read();
    }
  }
#endif

#ifdef TOUCH_PROBE_DEBUG
  if (gotData) {
    const int intPin = BoardConfig::ACTIVE.touch.irq;
    const int intState = (intPin >= 0) ? digitalRead(intPin) : -1;
    touchDebugPrintf("[touch] FT6336U poll ok int=%d mode=%02X gest=%02X cnt=%d ev=%d raw=[%02X %02X %02X %02X]\r\n",
                     intState, data[0], data[1], data[2] & 0x0F, (data[3] >> 6) & 0x03, data[3], data[4], data[5],
                     data[6]);
  } else {
    static uint32_t failCount = 0;
    if (++failCount % 200 == 1) {
      touchDebugPrintf("[touch] FT6336U poll fail (total=%lu)\r\n", static_cast<unsigned long>(failCount));
    }
  }
#endif
  if (!gotData) return;
  // Reject garbage frames. Pattern A: all four bytes identical (0xE6/E7/E2/01/03).
  // Pattern B: last three bytes identical but first differs (e.g. 07 03 03 03) —
  // produces rawX=1795 or rawY=771 which are impossibly out of range and generate
  // phantom touches at the corner of the screen.
  const bool uniformGarbage =
      (data[3] == data[4] && data[3] == data[5] && data[3] == data[6]) || (data[4] == data[5] && data[4] == data[6]);
  const int oneCount = (data[3] == 0x01) + (data[4] == 0x01) + (data[5] == 0x01) + (data[6] == 0x01);
  const bool stuckOneGarbage = data[0] == 0x01 && data[1] == 0x01 && (data[2] & 0x0F) == 0x01 && oneCount >= 3;
  if (uniformGarbage || stuckOneGarbage) {
    // Do not let a one-byte glitch latch the input loop into continuous reads.
    if (touchPressed && BoardConfig::ACTIVE.touch.irq >= 0 && digitalRead(BoardConfig::ACTIVE.touch.irq) != 0) {
      touchPressed = false;
      touchPoint.valid = false;
    }
    return;
  }

  const uint8_t numPoints = data[2] & 0x0F;
  const uint8_t eventFlag = (data[3] >> 6) & 0x03;  // 0=down, 1=up, 2=contact
  const bool isTouching = (numPoints > 0) && (eventFlag != 1);

  if (isTouching) {
    const uint16_t rawX = (static_cast<uint16_t>(data[3] & 0x0F) << 8) | data[4];
    const uint16_t rawY = (static_cast<uint16_t>(data[5] & 0x0F) << 8) | data[6];
    const auto& t = BoardConfig::ACTIVE.touch;
    const uint16_t sx = t.swapXY ? rawY : rawX;
    const uint16_t sy = t.swapXY ? rawX : rawY;
    // BoardConfig ranges describe the post-swap panel axes. Validate after the
    // mounting transform; validating rawY against rawMaxY rejected the lower
    // 320 rows of the portrait sensor.
    if (sx > static_cast<uint16_t>(t.rawMaxX) || sy > static_cast<uint16_t>(t.rawMaxY)) return;
    touchPoint.valid = true;
    touchPoint.x = mapTouchAxis(sx, t.rawMinX, t.rawMaxX, t.rawMaxX - t.rawMinX);
    touchPoint.y = mapTouchAxis(sy, t.rawMinY, t.rawMaxY, t.rawMaxY - t.rawMinY);
    if (t.flipX) touchPoint.x = static_cast<uint16_t>((t.rawMaxX - t.rawMinX) - touchPoint.x);
    if (t.flipY) touchPoint.y = static_cast<uint16_t>((t.rawMaxY - t.rawMinY) - touchPoint.y);
    touchPoint.timestamp = now;
    if (!touchPressed) {
      touchPressedEvent = true;
      touchDownPoint = touchPoint;
      touchMovedBeyondTapSlop = false;
    }
    touchUpPoint = touchPoint;
    const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
    const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
    if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
      touchMovedBeyondTapSlop = true;
    }
    touchPressed = true;
  } else {
    if (touchPressed) {
      touchReleasedEvent = true;
      lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
      touchUpPoint = touchPoint;
    }
    touchPressed = false;
    touchPoint.valid = false;
  }
}

void InputManager::pollGt911(const unsigned long now) {
  if (gt911Addr == 0) {
    return;
  }
  uint8_t status = 0;
  if (!gt911ReadReg(0x814E, &status, 1)) {
    // Keep the last complete frame while the single-touch state remains
    // latched. Clearing only this snapshot makes a transient I2C failure look
    // like a multi-contact release to multi-touch consumers, which can split one
    // physical gesture into two. A confirmed zero-contact frame below clears
    // the snapshot together with the rest of the touch state.
    return;
  }

  // Capacitive home key long-press (status bit 0x10). Fire from the LATCHED
  // down-state + wall clock, BEFORE the buffer-ready gate below: a motionless
  // hold stops producing new-data frames (0x80 stays clear), so gating the hold
  // timer on fresh frames would never let it cross the threshold. The
  // press/release EDGES still come from fresh frames (handled after the gate).
  if (touchHomeKeyDown && !touchHomeKeyLongFired && now - touchHomeKeyDownAt >= HOME_KEY_LONG_PRESS_MS) {
    touchHomeKeyLongEvent = true;  // crossed the threshold (a hold shortcut)
    touchHomeKeyLongFired = true;  // once per hold; also suppresses the release tap
  }

  if (!(status & 0x80)) {  // buffer not ready
    return;
  }

  // Home-key press/release edges (need a fresh frame). Short tap = primary
  // "home" action, fires on release; the long hold above suppresses it.
  const bool homeKeyDown = (status & 0x10) != 0;
  if (homeKeyDown && !touchHomeKeyDown) {  // press edge
    touchHomeKeyEvent = true;
    touchHomeKeyDownAt = now;
    touchHomeKeyLongFired = false;
  } else if (!homeKeyDown && touchHomeKeyDown && !touchHomeKeyLongFired) {
    touchHomeKeyTapEvent = true;  // release edge of a short press
  }
  touchHomeKeyDown = homeKeyDown;

  const uint8_t count = status & 0x0F;
  if (count > 0) {
    // GT911 stores each contact in a contiguous 8-byte record at 0x8150. Read
    // the bounded records in one transaction so every stored contact comes
    // from one coherent controller frame.
    const uint8_t storedCount = std::min<uint8_t>(count, MAX_TOUCH_CONTACTS);
    uint8_t points[MAX_TOUCH_CONTACTS * 8] = {};
    if (gt911ReadReg(0x8150, points, storedCount * 8)) {
      const auto& t = BoardConfig::ACTIVE.touch;
      touchSnapshot.reportedCount = count;
      touchSnapshot.idsStable = !t.gt911CoordsAtByte0;
      touchSnapshot.count = storedCount;
      for (uint8_t i = 0; i < touchSnapshot.count; ++i) {
        const uint8_t* record = points + i * 8;
        touchSnapshot.points[i].id = t.gt911CoordsAtByte0 ? i : (record[0] & 0x0F);
        const uint8_t offset = t.gt911CoordsAtByte0 ? 0 : 1;
        const uint16_t rawX = static_cast<uint16_t>(record[offset]) | (static_cast<uint16_t>(record[offset + 1]) << 8);
        const uint16_t rawY =
            static_cast<uint16_t>(record[offset + 2]) | (static_cast<uint16_t>(record[offset + 3]) << 8);
        TouchPoint& point = touchSnapshot.points[i].point;
        point.valid = true;
        // Panel-native coordinates (calibrated raw range, touch panel's
        // orientation); the app maps to its display/logical frame. Correct
        // digitizer mounting so the touch frame matches the display NATIVE
        // (panel) frame before any orientation mapping: swap axes first, then
        // map with the panel-axis ranges, then per-axis flip.
        const uint16_t sx = t.swapXY ? rawY : rawX;
        const uint16_t sy = t.swapXY ? rawX : rawY;
        point.x = mapTouchAxis(sx, t.rawMinX, t.rawMaxX, t.rawMaxX - t.rawMinX);
        point.y = mapTouchAxis(sy, t.rawMinY, t.rawMaxY, t.rawMaxY - t.rawMinY);
        if (t.flipX) point.x = static_cast<uint16_t>((t.rawMaxX - t.rawMinX) - point.x);
        if (t.flipY) point.y = static_cast<uint16_t>((t.rawMaxY - t.rawMinY) - point.y);
        point.timestamp = now;
      }

      // Gesture state consumes only completed controller frames. A failed
      // status/point read above deliberately does not arrive here, preserving
      // the active sequence through transient I2C failures.
      updateMultiTouchGesture(touchSnapshot, now);

      // Preserve the existing single-touch API from the first contact.
      touchPoint = touchSnapshot.points[0].point;
      if (!touchPressed) {
        touchPressedEvent = true;
        touchDownPoint = touchPoint;  // first contact sample, used for tap
                                      // routing (wasTouchTap)
        touchMovedBeyondTapSlop = false;
        touchMovedBeyondTapReleaseSlop = false;
      }
      touchUpPoint = touchPoint;
      const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
      const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
      if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
        touchMovedBeyondTapSlop = true;
      }
      if (absInt(dx) > TOUCH_TAP_RELEASE_SLOP_PX || absInt(dy) > TOUCH_TAP_RELEASE_SLOP_PX) {
        touchMovedBeyondTapReleaseSlop = true;
      }
      // A multi-contact gesture must not become a primary-contact tap in
      // existing single-touch consumers.
      if (touchSnapshot.reportedCount > 1) {
        touchMovedBeyondTapSlop = true;
        touchMovedBeyondTapReleaseSlop = true;
      }
#ifdef TOUCH_PROBE_DEBUG
      if (!touchPressed)
        touchDebugPrintf("[touch] press contacts=%u primary=(%u,%u)\n", touchSnapshot.count, touchPoint.x,
                         touchPoint.y);
#endif
      touchPressed = true;
    } else {
      // Retain the last complete snapshot until the controller provides an
      // authoritative contact count. The existing single-touch state is also
      // retained on this transient point-read failure.
    }
  } else {
    touchSnapshot.count = 0;
    touchSnapshot.reportedCount = 0;
    touchSnapshot.idsStable = !BoardConfig::ACTIVE.touch.gt911CoordsAtByte0;
    updateMultiTouchGesture(touchSnapshot, now);
    if (touchPressed) {
      touchReleasedEvent = true;
      lastTouchHeldDurationMs = now - touchDownPoint.timestamp;
      touchUpPoint = touchPoint;  // last contact sample, used for swipe routing
    }
    touchPressed = false;
    touchPoint.valid = false;
  }

  gt911ClearStatus();  // GT911 requires clearing 0x814E after each read
}

#endif  // FREEINK_CAP_TOUCH
