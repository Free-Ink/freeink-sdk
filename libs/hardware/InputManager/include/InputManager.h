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

class InputManager {
 public:
  InputManager();
  void begin();
  uint8_t getState();

  /**
   * Updates the button states. Should be called regularly in the main loop.
   */
  void update();

  /**
   * Returns true if the button was being held at the time of the last #update() call.
   *
   * @param buttonIndex the button indexes
   * @return the button current press state
   */
  bool isPressed(uint8_t buttonIndex) const;

 /**
   * Returns true if the button went from unpressed to pressed between the last two #update() calls.
   *
   * This differs from #isPressed() in that pressing and holding a button will cause this function
   * to return true after the first #update() call, but false on subsequent calls, whereas #isPressed()
   * will continue to return true.
   *
   * @param buttonIndex
   * @return the button pressed state
   */
  bool wasPressed(uint8_t buttonIndex) const;

  /**
   * Returns true if any button started being pressed between the last two #update() calls
   *
   * @return true if any button started being pressed between the last two #update() calls
   */
  bool wasAnyPressed() const;

  /**
   * Returns true if the button went from pressed to unpressed between the last two #update() calls
   *
   * @param buttonIndex the button indexes
   * @return the button release state
   */
  bool wasReleased(uint8_t buttonIndex) const;

  /**
   * Returns true if any button was released between the last two #update() calls
   *
   * @return  true if any button was released between the last two #update() calls
   */
  bool wasAnyReleased() const;

  /**
   * Returns the time between any button starting to be depressed and all buttons between released
   *
   * @return duration in milliseconds
   */
  unsigned long getHeldTime() const;

  /**
   * Returns the time the power button has been held.
   */
  unsigned long getPowerButtonHeldTime() const;

  // Button indices
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

  // Power button methods
  bool isPowerButtonPressed() const;

  // Button names
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
  // True on the press edge of the GT911 capacitive home key (controllers without
  // one never report it). Cleared each #update().
  bool wasHomeKeyPressed() const;

  // Optional board hook for buttons that aren't direct GPIOs — e.g. a key behind
  // an I2C IO-expander (the LilyGo T5 S3 user button on its PCA9535). It returns
  // a (1<<BTN_*) bitmask that is OR'd into every update(); the board reads its
  // expander, so InputManager itself stays device-agnostic. Default: none.
  using ButtonHook = uint8_t (*)();
  static void setButtonHook(ButtonHook hook) { s_buttonHook = hook; }

 private:
  static ButtonHook s_buttonHook;

  int getButtonFromADC(int adcValue, const int ranges[], int numButtons);
  bool isDigitalPressed(int8_t pin) const;
  uint8_t getDigitalState() const;
  void updateConfirmBackHold(unsigned long currentTime);
  void applyStateChange(uint8_t state, unsigned long currentTime);

  // Touch backend. Compiled only when FREEINK_CAP_TOUCH is set; dispatches on
  // BoardConfig::ACTIVE.touch.controller (CHSC6x IRQ-driven, GT911 polled).
  void beginTouch();
  uint8_t serviceTouch();                                   // runs the machine; returns synthesized button mask
  bool touchIrqActive(int irqRaw) const;
  void updateTouchFromIrq(unsigned long now, int irqRaw);   // CHSC6x IRQ state machine
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

  // Touch state machine
  bool touchIrqEnabled = false;       // IRQ pin configured
  bool touchDataEnabled = false;      // I2C up, controller present
  uint8_t gt911Addr = 0;              // resolved GT911 address (0 until probed)
  int touchIrqLast = 1;
  unsigned long touchIrqLastChangeTime = 0;
  unsigned long touchIrqPulseUntil = 0;
  bool touchReadPending = false;
  unsigned long touchReadAt = 0;
  unsigned long touchReleaseAt = 0;
  bool touchPressed = false;
  bool touchPressedEvent = false;
  bool touchReleasedEvent = false;
  bool touchHomeKeyEvent = false;  // GT911 capacitive home key, press edge
  bool touchHomeKeyDown = false;
  TouchPoint touchPoint = {false, 0, 0, 0};

  static constexpr int NUM_BUTTONS_1 = 4;
  static const int ADC_RANGES_1[];

  static constexpr int NUM_BUTTONS_2 = 2;
  static const int ADC_RANGES_2[];

  static constexpr int ADC_NO_BUTTON = 3900;
  static constexpr unsigned long DEBOUNCE_DELAY = 5;
  static constexpr unsigned long CONFIRM_BACK_HOLD_MS = 650;

  // Touch timing / protocol constants (ported from the Murphy M3 CHSC6x driver).
  static constexpr unsigned long TOUCH_IRQ_DEBOUNCE_MS = 5;
  static constexpr unsigned long TOUCH_IRQ_PULSE_MS = 120;
  static constexpr unsigned long TOUCH_SAMPLE_DELAY_MS = 8;
  static constexpr uint8_t TOUCH_READ_COMMAND = 0x00;
  static constexpr uint8_t TOUCH_FRAME_SIZE = 16;

  static const char* BUTTON_NAMES[];
};
