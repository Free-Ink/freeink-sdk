#pragma once

// Metalio E-Ink4-Plus (ESP32-S31), TCA9555 at 0x20 on SDA0/SCL1. Pins follow the
// vendor's config.h for board rev 1.3/1.4. Expander indices 8..15 mean P1.0..P1.7,
// not ESP GPIO numbers. The EPD glue (libs/hardware/MetalioPlusBoard) drives the
// TPS65185 lines through setOutputs() so this header is the expander's one owner.
#include <Arduino.h>
#include <Wire.h>
#include <mutex>
#include "I2cBusLock.h"

namespace freeink {
namespace metalioplus {
constexpr int I2C_SDA = 0;
constexpr int I2C_SCL = 1;
constexpr uint32_t I2C_HZ = 400000;
constexpr uint8_t EXPANDER_ADDR = 0x20;

// Port 0.
constexpr uint8_t PIN_TPS_VCOM_CTRL = 0;
constexpr uint8_t PIN_TPS_PWRUP = 1;
constexpr uint8_t PIN_TPS_WAKEUP = 2;
constexpr uint8_t PIN_MOTOR = 3;
constexpr uint8_t PIN_AMP_ENABLE = 4;
constexpr uint8_t PIN_SCREEN_POWER = 5;  // shared with the camera
constexpr uint8_t PIN_TPS_PWR_GOOD = 7;  // input
// Port 1.
constexpr uint8_t PIN_VOLUME_UP = 8;
constexpr uint8_t PIN_VOLUME_DOWN = 9;
constexpr uint8_t PIN_TOUCH_RESET = 10;  // high = running; shared with the 4G modem
constexpr uint8_t PIN_POWER_PULSE = 11;  // power-switch pulse, idle low
constexpr uint8_t PIN_RADIO_POWER = 12;  // 4G, Bluetooth audio and amplifier rail
constexpr uint8_t PIN_RTC_INT = 14;      // input
constexpr uint8_t PIN_POWER_KEY = 15;    // input
// The expander's open-drain INT, wired-AND with the SC7A20H INT1 (vendor config.h).
// Any change on an expander input pulls it low until the input port is read.
constexpr gpio_num_t EXPANDER_INT_GPIO = GPIO_NUM_2;

constexpr uint16_t OUTPUTS = (1u << PIN_TPS_VCOM_CTRL) | (1u << PIN_TPS_PWRUP) | (1u << PIN_TPS_WAKEUP) |
                             (1u << PIN_MOTOR) | (1u << PIN_AMP_ENABLE) | (1u << PIN_SCREEN_POWER) |
                             (1u << PIN_TOUCH_RESET) | (1u << PIN_POWER_PULSE) | (1u << PIN_RADIO_POWER);
constexpr uint16_t KEYS = (1u << PIN_VOLUME_UP) | (1u << PIN_VOLUME_DOWN) | (1u << PIN_POWER_KEY);

inline bool writeRegister(uint8_t reg, uint16_t value) {
  const freeink::I2cBusLock busLock;
  Wire.beginTransmission(EXPANDER_ADDR);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value));
  Wire.write(static_cast<uint8_t>(value >> 8));
  return Wire.endTransmission() == 0;
}

inline bool readRegister(uint8_t reg, uint16_t& value) {
  const freeink::I2cBusLock busLock;
  Wire.beginTransmission(EXPANDER_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(EXPANDER_ADDR, uint8_t(2), uint8_t(true)) != 2) return false;
  const uint8_t lo = Wire.read();
  value = lo | (static_cast<uint16_t>(Wire.read()) << 8);
  return true;
}

// Output latch as last written. The panel power sequence changes these lines
// several times per frame, so writes come from this copy instead of a re-read.
inline uint16_t& outputLatch() {
  static uint16_t latch = (1u << PIN_SCREEN_POWER) | (1u << PIN_TOUCH_RESET) | (1u << PIN_RADIO_POWER);
  return latch;
}

inline std::mutex& outputMutex() {
  static std::mutex mutex;
  return mutex;
}

// Sets the expander outputs in `mask` to the bits of `value`. Skips the bus when
// nothing changes. Call from tasks, never an ISR.
inline bool setOutputs(uint16_t mask, uint16_t value) {
  std::lock_guard<std::mutex> lock(outputMutex());
  uint16_t& latch = outputLatch();
  const uint16_t next = (latch & ~mask) | (value & mask);
  if (next == latch) return true;
  if (!writeRegister(2, next)) return false;
  latch = next;
  return true;
}

inline bool setOutput(uint8_t pin, bool high) { return setOutputs(1u << pin, high ? 1u << pin : 0); }

inline bool readInputs(uint16_t& value) { return readRegister(0, value); }

// Powers the screen rail and the radio rail, releases touch (and 4G modem)
// reset and leaves the TPS65185 asleep. The radio rail (P1.4) also feeds the 4G
// modem, Bluetooth audio module and amplifier, which nothing here drives, but it
// cannot be left off: with it low the TPS65185 stops answering at 0x68 and the
// panel cannot be powered (measured). Retries are rate-limited so a missing
// expander does not stall polls.
inline bool ensureBooted(uint32_t waitMs = 0) {
  constexpr unsigned long RETRY_GAP_MS = 500;
  static bool ready = false;
  static bool tried = false;
  static unsigned long lastFailure = 0;
  if (ready) return true;
  freeink::enableI2cBusLock();  // first call runs in setup(), before the render task exists
  if (waitMs == 0 && tried && millis() - lastFailure < RETRY_GAP_MS) return false;
  tried = true;
  // Holds latched for deep sleep survive a CPU reset and would pin the bus.
  gpio_hold_dis(static_cast<gpio_num_t>(I2C_SDA));
  gpio_hold_dis(static_cast<gpio_num_t>(I2C_SCL));
  const unsigned long start = millis();
  for (;;) {
    {
      const freeink::I2cBusLock busLock;  // begin must not race a transfer
      if (Wire.begin(I2C_SDA, I2C_SCL, I2C_HZ)) {
        Wire.setTimeOut(10);
        uint16_t probe;
        if (readRegister(6, probe)) break;
      }
    }
    if (millis() - start >= waitMs) {
      lastFailure = millis();
      return false;
    }
    delay(50);
  }
  // Touch reset low first so the release below is a clean reset pulse. Levels
  // before directions, so no output glitches as it turns into an output.
  std::lock_guard<std::mutex> lock(outputMutex());
  uint16_t& latch = outputLatch();
  latch = (1u << PIN_SCREEN_POWER) | (1u << PIN_RADIO_POWER);
  if (!writeRegister(2, latch) || !writeRegister(4, 0) || !writeRegister(6, static_cast<uint16_t>(~OUTPUTS))) {
    lastFailure = millis();
    return false;
  }
  delay(10);
  latch |= 1u << PIN_TOUCH_RESET;
  if (!writeRegister(2, latch)) {
    lastFailure = millis();
    return false;
  }
  delay(300);  // FT6336U needs 300 ms after reset (vendor Trsi)
  ready = true;
  return true;
}

// Pressed keys as set bits (PIN_VOLUME_UP etc.). The keys are pulled up and read
// low when pressed (vendor metalio_keys.h); a polarity learned from a boot sample
// would invert if the power key were still held from switching the board on.
// Isolated failed reads keep the last sample; reads failing for longer than
// HOLD_MS release the keys.
inline uint16_t readPressedKeys() {
  constexpr unsigned long HOLD_MS = 100;
  static unsigned long sampledAt = 0;
  static unsigned long goodAt = 0;
  static uint16_t pressed = 0;
  static bool sampled = false;
  const unsigned long now = millis();
  if (sampled && now - sampledAt < 20) return pressed;
  sampled = true;
  sampledAt = now;
  uint16_t inputs;
  if (ensureBooted() && readInputs(inputs)) {
    pressed = ~inputs & KEYS;
    goodAt = now;
  } else if (now - goodAt > HOLD_MS) {
    pressed = 0;
  }
  return pressed;
}

// Call only after display.deepSleep() and storage shutdown. The vendor firmware
// toggles the power-switch pulse line (P1.3) every 100 ms, 15 times, until the
// power-switch chip cuts power, which it does with USB connected too (measured).
// It returns only if the switch did not respond.
inline void powerOff() {
  if (!ensureBooted()) return;
  setOutputs((1u << PIN_MOTOR) | (1u << PIN_AMP_ENABLE), 0);
  for (int i = 0; i < 15; ++i) {
    setOutput(PIN_POWER_PULSE, true);
    delay(100);
    setOutput(PIN_POWER_PULSE, false);
    delay(100);
  }
}

// Stays awake on the sleep screen and restarts on the next power-key press:
// the last resort when neither the power cut nor a key-wake deep sleep works.
[[noreturn]] inline void waitForPowerKeyThenRestart() {
  while (readPressedKeys() & (1u << PIN_POWER_KEY)) delay(50);  // the press that slept
  for (;;) {
    if (readPressedKeys() & (1u << PIN_POWER_KEY)) esp_restart();
    delay(100);
  }
}

// Readies a deep sleep that wakes on the expander's INT (GPIO2): waits for the
// power key that requested sleep to be released, then reads the input port to
// release INT. Returns true when INT idles high; INT held low (a key down, or
// the accelerometer's INT1 asserting) would end the sleep at once. Any expander
// input can wake the board this way, so check powerKeyHeld() after the wake.
inline bool prepareKeyWake() {
  while (readPressedKeys() & (1u << PIN_POWER_KEY)) delay(50);  // the press that slept
  delay(50);
  uint16_t inputs;
  readInputs(inputs);
  pinMode(EXPANDER_INT_GPIO, INPUT_PULLUP);
  return digitalRead(EXPANDER_INT_GPIO) == HIGH;
}

// True when the power key is held now; for checking a deep-sleep wake.
inline bool powerKeyHeld() { return (readPressedKeys() & (1u << PIN_POWER_KEY)) != 0; }
}  // namespace metalioplus
}  // namespace freeink
