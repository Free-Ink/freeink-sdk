#include "InputManager.h"

#if FREEINK_CAP_TOUCH
#include <Wire.h>
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
      currentTouch{false, 0, 0, 0},
      lastTouchValid(false) {}

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

uint8_t InputManager::getState() {
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    return getDigitalState();
  }

  uint8_t state = 0;

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

  return state;
}

bool InputManager::isDigitalPressed(const int8_t pin) const {
  return pin >= 0 && digitalRead(pin) == LOW;
}

uint8_t InputManager::getDigitalState() const {
  uint8_t state = 0;

  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmBackHold) {
    if (isDigitalPressed(BoardConfig::ACTIVE.input.back)) state |= (1 << BTN_BACK);
    if (isDigitalPressed(BoardConfig::ACTIVE.input.confirm)) state |= (1 << BTN_CONFIRM);
  }

  if (isDigitalPressed(BoardConfig::ACTIVE.input.left)) state |= (1 << BTN_LEFT);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.right)) state |= (1 << BTN_RIGHT);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.up)) state |= (1 << BTN_UP);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.down)) state |= (1 << BTN_DOWN);
  if (isDigitalPressed(BoardConfig::ACTIVE.input.power) &&
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmBackHold) {
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

void InputManager::update() {
  const unsigned long currentTime = millis();

  pressedEvents = 0;
  releasedEvents = 0;

  updateTouch(currentTime);

  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalConfirmBackHold) {
    updateConfirmBackHold(currentTime);
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

bool InputManager::isPowerButtonPressed() const {
  return isPressed(BTN_POWER);
}

// ============================================================================
// Capacitive touch
//
// The public touch API is always available; the backend is a no-op on boards
// whose BoardConfig::ACTIVE.touch.controller is None. CHSC6x / GT911 frame
// decoding lands with the touch-capable device drivers (e.g. Murphy M3).
// ============================================================================

bool InputManager::hasTouch() const {
#if FREEINK_CAP_TOUCH
  return BoardConfig::ACTIVE.touch.controller != BoardConfig::TouchController::None;
#else
  return false;  // touch code not compiled in (FREEINK_CAP_TOUCH=0)
#endif
}

InputManager::TouchPoint InputManager::getTouchPoint() const { return currentTouch; }

bool InputManager::isTouchPressed() const { return currentTouch.valid; }

bool InputManager::wasTouchPressed() const { return currentTouch.valid && !lastTouchValid; }

void InputManager::beginTouch() {
#if FREEINK_CAP_TOUCH
  const auto& t = BoardConfig::ACTIVE.touch;
  if (t.controller == BoardConfig::TouchController::None) {
    return;
  }
  if (t.sda >= 0 && t.scl >= 0) {
    Wire.begin(t.sda, t.scl, 100000);
  }
  if (t.reset >= 0) {
    pinMode(t.reset, OUTPUT);
    digitalWrite(t.reset, HIGH);
  }
  if (t.irq >= 0) {
    pinMode(t.irq, INPUT_PULLUP);
  }
#endif
}

void InputManager::updateTouch(const unsigned long currentTime) {
#if FREEINK_CAP_TOUCH
  if (BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::None) {
    return;
  }

  lastTouchValid = currentTouch.valid;
  TouchPoint sampled{false, 0, 0, currentTime};
  if (readTouchPoint(sampled)) {
    sampled.timestamp = currentTime;
    currentTouch = sampled;
    if (BoardConfig::ACTIVE.touch.synthesizeConfirm && !lastTouchValid) {
      pressedEvents |= (1 << BTN_CONFIRM);
      releasedEvents |= (1 << BTN_CONFIRM);
    }
  } else {
    currentTouch.valid = false;
  }
#else
  (void)currentTime;
#endif
}

bool InputManager::readTouchPoint(TouchPoint& out) {
  // Backend stub: concrete CHSC6x / GT911 I2C frame decode is added with the
  // touch-capable panel drivers (and only when FREEINK_CAP_TOUCH is enabled).
  (void)out;
  return false;
}
