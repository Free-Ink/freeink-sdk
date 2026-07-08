#include "InputManager.h"

#if FREEINK_CAP_TOUCH
#include <Wire.h>
#include <driver/gpio.h>
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
// Setup ranges, if ADC value is between value `i` and `i + 1`, button `i` is being pressed.
// These ranges are based on real world values above, and are much more tolerant of different
// devices than a fixed threshold check. They are calculated by taking the midpoint of the
// pairs of averaged values above.
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
      confirmPowerLongPressActive(false) {}

void InputManager::begin() {
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder) {
    pinMode(BUTTON_ADC_PIN_1, INPUT);
    pinMode(BUTTON_ADC_PIN_2, INPUT);
    pinMode(BoardConfig::ACTIVE.input.power,
            BoardConfig::ACTIVE.input.powerActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
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
    state |= serviceTouch();  // run the touch machine; OR any synthesized button
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
  xTaskCreate(asyncTaskTrampoline, "fi_input", 4096, this, taskPriority, &_asyncTask);
}

void InputManager::asyncTaskTrampoline(void* self) { static_cast<InputManager*>(self)->asyncPoll(); }

void InputManager::asyncPoll() {
  static const uint8_t kButtons[] = {BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN};
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

bool InputManager::isDigitalPressed(const int8_t pin) const {
  return pin >= 0 && digitalRead(pin) == LOW;
}

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

void InputManager::update() {
  const unsigned long currentTime = millis();

  pressedEvents = 0;
  releasedEvents = 0;
  touchPressedEvent = false;   // one-shot touch coord events, cleared each update()
  touchReleasedEvent = false;
  touchHomeKeyEvent = false;

  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalConfirmBackHold) {
    updateConfirmBackHold(currentTime);
    return;
  }
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    updateConfirmPowerHold(currentTime);
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

bool InputManager::isPressed(const uint8_t buttonIndex) const {
  return currentState & (1 << buttonIndex);
}

bool InputManager::wasPressed(const uint8_t buttonIndex) const {
  return pressedEvents & (1 << buttonIndex);
}

bool InputManager::wasAnyPressed() const {
  return pressedEvents > 0;
}

bool InputManager::wasReleased(const uint8_t buttonIndex) const {
  return releasedEvents & (1 << buttonIndex);
}

bool InputManager::wasAnyReleased() const {
  return releasedEvents > 0;
}

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

bool InputManager::isPowerButtonPressed() const {
  return isPressed(BTN_POWER);
}

// ============================================================================
// Capacitive touch
//
// The public touch API is always available. Compiled only when FREEINK_CAP_TOUCH
// is set; the backend dispatches on BoardConfig::ACTIVE.touch.controller:
//   * CHSC6x (Murphy M3) — IRQ-driven, hand-rolled 16-byte frame decode.
//   * GT911  (LilyGo)    — polled status/point registers over I2C.
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
bool InputManager::isTouchPressed() const { return touchPressed; }
bool InputManager::wasTouchPressed() const { return touchPressedEvent; }
bool InputManager::wasTouchReleased() const { return touchReleasedEvent; }

bool InputManager::wasTouchTap(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  if (!touchReleasedEvent) return false;
  if (touchMovedBeyondTapSlop) return false;
  // Tap position = the FIRST contact sample (touch-down), not the last: the
  // reported centroid drifts 10-20px as a finger rolls off during lift, which
  // made small targets (steppers) feel unreliable with release-point routing.
  // A tap routes to where the user touched, not where the finger let go.
  const auto& t = BoardConfig::ACTIVE.touch;
  const uint16_t w = (t.rawMaxX > t.rawMinX) ? static_cast<uint16_t>(t.rawMaxX - t.rawMinX) : 1;
  const uint16_t h = (t.rawMaxY > t.rawMinY) ? static_cast<uint16_t>(t.rawMaxY - t.rawMinY) : 1;
  float x = static_cast<float>(touchDownPoint.x) / w;
  float y = static_cast<float>(touchDownPoint.y) / h;
  nx = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
  ny = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

bool InputManager::wasTouchPressedAt(float& nx, float& ny) const {
#if FREEINK_CAP_TOUCH
  // Press-edge analogue of wasTouchTap: true on the frame a touch begins, writing
  // the touch-down position normalized 0..1 in the panel's native frame. Lets the
  // app highlight what's under the finger on touch-down (before release).
  if (!touchPressedEvent) return false;
  const auto& t = BoardConfig::ACTIVE.touch;
  const uint16_t w = (t.rawMaxX > t.rawMinX) ? static_cast<uint16_t>(t.rawMaxX - t.rawMinX) : 1;
  const uint16_t h = (t.rawMaxY > t.rawMinY) ? static_cast<uint16_t>(t.rawMaxY - t.rawMinY) : 1;
  float x = static_cast<float>(touchDownPoint.x) / w;
  float y = static_cast<float>(touchDownPoint.y) / h;
  nx = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
  ny = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
  return true;
#else
  (void)nx;
  (void)ny;
  return false;
#endif
}

bool InputManager::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
#if FREEINK_CAP_TOUCH
  if (!touchPressed || touchMovedBeyondTapSlop) return false;
  const auto& t = BoardConfig::ACTIVE.touch;
  const uint16_t w = (t.rawMaxX > t.rawMinX) ? static_cast<uint16_t>(t.rawMaxX - t.rawMinX) : 1;
  const uint16_t h = (t.rawMaxY > t.rawMinY) ? static_cast<uint16_t>(t.rawMaxY - t.rawMinY) : 1;
  float x = static_cast<float>(touchDownPoint.x) / w;
  float y = static_cast<float>(touchDownPoint.y) / h;
  nx = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
  ny = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
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
  if (!touchPressed) return false;
  const auto& t = BoardConfig::ACTIVE.touch;
  const uint16_t w = (t.rawMaxX > t.rawMinX) ? static_cast<uint16_t>(t.rawMaxX - t.rawMinX) : 1;
  const uint16_t h = (t.rawMaxY > t.rawMinY) ? static_cast<uint16_t>(t.rawMaxY - t.rawMinY) : 1;
  float x = static_cast<float>(touchUpPoint.x) / w;
  float y = static_cast<float>(touchUpPoint.y) / h;
  nx = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
  ny = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
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
  if (!touchReleasedEvent) return false;
  // A flick: travelled past a distance threshold within a time window. Distance is
  // measured in native px; the dominant axis is left to the app (after mapping to
  // its logical frame).
  if (lastTouchHeldDurationMs > TOUCH_SWIPE_MAX_MS) return false;
  const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
  const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
  const int adx = absInt(dx);
  const int ady = absInt(dy);
  if (adx < TOUCH_SWIPE_MIN_PX && ady < TOUCH_SWIPE_MIN_PX) return false;
  const auto& t = BoardConfig::ACTIVE.touch;
  const uint16_t w = (t.rawMaxX > t.rawMinX) ? static_cast<uint16_t>(t.rawMaxX - t.rawMinX) : 1;
  const uint16_t h = (t.rawMaxY > t.rawMinY) ? static_cast<uint16_t>(t.rawMaxY - t.rawMinY) : 1;
  auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
  nxStart = clamp01(static_cast<float>(touchDownPoint.x) / w);
  nyStart = clamp01(static_cast<float>(touchDownPoint.y) / h);
  nxEnd = clamp01(static_cast<float>(touchUpPoint.x) / w);
  nyEnd = clamp01(static_cast<float>(touchUpPoint.y) / h);
  return true;
#else
  (void)nxStart;
  (void)nyStart;
  (void)nxEnd;
  (void)nyEnd;
  return false;
#endif
}

bool InputManager::wasHomeKeyPressed() const { return touchHomeKeyEvent; }

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

  if (t.controller == BoardConfig::TouchController::Gt911) {
    pollGt911(now);
  } else {
    updateTouchFromIrq(now, 0);  // detection polls I2C; the IRQ is unused now
    // Synthesized confirm tracks an actually-detected press, not the IRQ line.
    if (touchPressedEvent) touchIrqPulseUntil = now + TOUCH_IRQ_PULSE_MS;
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
      } else {
        touchUpPoint = point;
        const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
        const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
        if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
          touchMovedBeyondTapSlop = true;
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
  // Panel-native coordinates (the calibrated raw range, in the touch panel's own
  // orientation); the app maps to its display/logical frame. See the touch note
  // in the README.
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

// --- GT911 (LilyGo) ---------------------------------------------------------

void InputManager::beginGt911() {
  const auto& t = BoardConfig::ACTIVE.touch;

  // Power the touch rail first (boards that gate it, e.g. Sticky's TOUCH_EN on
  // GPIO42). Active-high + settle, before the reset dance and I2C probe; without
  // this the GT911 never ACKs and touch is reported absent. No-op when unassigned.
  // gpio_hold_dis first: the sleep path holds this pin LOW and the hold survives
  // the deep-sleep wake reset; the HIGH write is a no-op until it is released.
  if (t.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(t.powerEnable));
    pinMode(t.powerEnable, OUTPUT);
    digitalWrite(t.powerEnable, HIGH);
    delay(50);
  }

  if (t.sda >= 0 && t.scl >= 0) {
    Wire.begin(t.sda, t.scl, 400000);
    Wire.setTimeOut(10);
  }

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
  // Boards differ in which strapped address survives their module wiring, so try
  // the primary-select level first, then the alternate level before declaring the
  // touch controller absent.
  gt911Addr = 0;
  resetWithIntLevel(LOW);
  if (!probeCandidates()) {
    resetWithIntLevel(HIGH);
    probeCandidates();
  }

  touchDataEnabled = (gt911Addr != 0);
#ifdef TOUCH_PROBE_DEBUG
  touchDebugPrintf("[touch] GT911 probe: addr=0x%02X enabled=%d (sda=%d scl=%d cand=0x%02X/0x%02X)\n", gt911Addr,
                   touchDataEnabled, t.sda, t.scl, t.i2cAddress, t.i2cAddressAlt);
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

void InputManager::pollGt911(const unsigned long now) {
  if (gt911Addr == 0) {
    return;
  }
  uint8_t status = 0;
  if (!gt911ReadReg(0x814E, &status, 1)) {
    return;
  }
  if (!(status & 0x80)) {  // buffer not ready
    return;
  }

  // Capacitive home key (status bit 0x10): emit one event on the press edge.
  const bool homeKeyDown = (status & 0x10) != 0;
  if (homeKeyDown && !touchHomeKeyDown) touchHomeKeyEvent = true;
  touchHomeKeyDown = homeKeyDown;

  const uint8_t count = status & 0x0F;
  if (count > 0) {
    uint8_t pt[8] = {};
    if (gt911ReadReg(0x8150, pt, 8)) {
      // Coordinate bytes start at 0 (no track-id, e.g. M5Paper) or 1 (datasheet
      // standard, e.g. LilyGo) depending on the board's GT911 config.
      const uint8_t o = BoardConfig::ACTIVE.touch.gt911CoordsAtByte0 ? 0 : 1;
      const uint16_t rawX = static_cast<uint16_t>(pt[o]) | (static_cast<uint16_t>(pt[o + 1]) << 8);
      const uint16_t rawY = static_cast<uint16_t>(pt[o + 2]) | (static_cast<uint16_t>(pt[o + 3]) << 8);
      const auto& t = BoardConfig::ACTIVE.touch;
      touchPoint.valid = true;
      // Panel-native coordinates (calibrated raw range, touch panel's orientation);
      // the app maps to its display/logical frame.
      // Correct digitizer mounting so the touch frame matches the display NATIVE
      // (panel) frame before any orientation mapping: swap axes first (rotated 90°
      // sensor), then map with the panel-axis ranges, then per-axis flip.
      const uint16_t sx = t.swapXY ? rawY : rawX;
      const uint16_t sy = t.swapXY ? rawX : rawY;
      touchPoint.x = mapTouchAxis(sx, t.rawMinX, t.rawMaxX, t.rawMaxX - t.rawMinX);
      touchPoint.y = mapTouchAxis(sy, t.rawMinY, t.rawMaxY, t.rawMaxY - t.rawMinY);
      if (t.flipX) touchPoint.x = static_cast<uint16_t>((t.rawMaxX - t.rawMinX) - touchPoint.x);
      if (t.flipY) touchPoint.y = static_cast<uint16_t>((t.rawMaxY - t.rawMinY) - touchPoint.y);
      touchPoint.timestamp = now;
      if (!touchPressed) {
        touchPressedEvent = true;
        touchDownPoint = touchPoint;  // first contact sample, used for tap routing (wasTouchTap)
        touchMovedBeyondTapSlop = false;
      }
      touchUpPoint = touchPoint;
      const int dx = static_cast<int>(touchUpPoint.x) - static_cast<int>(touchDownPoint.x);
      const int dy = static_cast<int>(touchUpPoint.y) - static_cast<int>(touchDownPoint.y);
      if (absInt(dx) > TOUCH_TAP_SLOP_PX || absInt(dy) > TOUCH_TAP_SLOP_PX) {
        touchMovedBeyondTapSlop = true;
      }
#ifdef TOUCH_PROBE_DEBUG
      if (!touchPressed)
        touchDebugPrintf("[touch] press pt=[%02X %02X %02X %02X %02X %02X %02X %02X] raw=(%u,%u) mapped=(%u,%u)\n",
                         pt[0], pt[1], pt[2], pt[3], pt[4], pt[5], pt[6], pt[7], rawX, rawY, touchPoint.x,
                         touchPoint.y);
#endif
      touchPressed = true;
    }
  } else {
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
