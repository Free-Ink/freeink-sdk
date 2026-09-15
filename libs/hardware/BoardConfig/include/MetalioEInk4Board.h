#pragma once

// Metalio E-Ink 4, TCA9555 at 0x20 on SDA41/SCL42.
// Expander indices 8..15 mean P1.0..P1.7, not ESP GPIO numbers.
#include <Arduino.h>
#include <Wire.h>

namespace freeink {
namespace metalio {
constexpr uint8_t EXPANDER_ADDR = 0x20;
constexpr uint8_t PIN_AMP_SELECT = 1;
constexpr uint8_t PIN_AMP_ENABLE = 4;
constexpr uint8_t PIN_SCREEN_SD_POWER = 5;
constexpr uint8_t PIN_MAIN_POWER = 6;
constexpr uint8_t PIN_VOLUME_DOWN = 7;
constexpr uint8_t PIN_VOLUME_UP = 8;
constexpr uint8_t PIN_TOUCH_RESET = 9;
constexpr uint8_t PIN_POWER_PULSE = 11;
constexpr int VIBRATION_GPIO = 44;

inline bool writeRegister(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(EXPANDER_ADDR);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value));
  Wire.write(static_cast<uint8_t>(value >> 8));
  return Wire.endTransmission() == 0;
}

inline bool readRegister(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(EXPANDER_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(EXPANDER_ADDR, uint8_t(2), uint8_t(true)) != 2) return false;
  const uint8_t lo = Wire.read();
  value = lo | (static_cast<uint16_t>(Wire.read()) << 8);
  return true;
}

// Read/modify/write preserves unrelated expander outputs. Call board functions
// from the firmware's hardware task, never an ISR.
inline bool setOutput(uint8_t pin, bool high) {
  if (pin > 15) return false;
  uint16_t value;
  if (!readRegister(2, value)) return false;
  const uint16_t mask = uint16_t(1) << pin;
  return writeRegister(2, high ? value | mask : value & ~mask);
}

inline bool ensureBooted() {
  static bool ready = false;
  if (ready) return true;
  if (!Wire.begin(41, 42, 400000)) return false;
  Wire.setTimeOut(10);
  constexpr uint16_t outputs = (1u << PIN_AMP_SELECT) | (1u << PIN_AMP_ENABLE) |
      (1u << PIN_SCREEN_SD_POWER) | (1u << PIN_MAIN_POWER) |
      (1u << PIN_TOUCH_RESET) | (1u << PIN_POWER_PULSE);
  constexpr uint16_t inputs = (1u << PIN_VOLUME_DOWN) | (1u << PIN_VOLUME_UP) | (1u << 12);
  uint16_t latch, config, polarity;
  if (!readRegister(2, latch) || !readRegister(6, config) || !readRegister(4, polarity)) return false;
  // Set safe levels BEFORE switching directions: no shutdown pulse or amp pop.
  latch = (latch & ~outputs) | (1u << PIN_MAIN_POWER) | (1u << PIN_POWER_PULSE);
  if (!writeRegister(2, latch) || !writeRegister(6, (config & ~outputs) | inputs) ||
      !writeRegister(4, polarity & ~inputs)) return false;
  if (!setOutput(PIN_SCREEN_SD_POWER, true)) return false;
  delay(10);
  if (!setOutput(PIN_TOUCH_RESET, true)) return false;
  delay(120);
  pinMode(2, INPUT_PULLUP);
  pinMode(VIBRATION_GPIO, OUTPUT);
  digitalWrite(VIBRATION_GPIO, LOW);
  ready = true;
  return true;
}

// A failed read releases keys instead of retaining a stale pressed state.
inline uint16_t readButtons() {
  static unsigned long sampledAt = 0;
  static uint16_t value = 0xFFFF;
  static bool sampled = false;
  const unsigned long now = millis();
  if (sampled && now - sampledAt < 20) return value;
  sampled = true;
  sampledAt = now;
  if (!ensureBooted() || !readRegister(0, value)) value = 0xFFFF;
  return value;
}

inline bool setAmplifier(bool enabled) {
  return ensureBooted() && setOutput(PIN_AMP_SELECT, false) && setOutput(PIN_AMP_ENABLE, enabled);
}

inline void vibrate(uint16_t durationMs = 35) {
  if (!ensureBooted()) return;
  digitalWrite(VIBRATION_GPIO, HIGH);
  delay(durationMs);
  digitalWrite(VIBRATION_GPIO, LOW);
}

// Call only after display.deepSleep() and storage shutdown. Keep the shared
// screen/card rail on while the panel's high-voltage supplies discharge.
// One pulse per call; firmware may repeat if USB keeps the power controller alive.
inline bool powerOff() {
  if (!ensureBooted() || !setAmplifier(false)) return false;
  delay(280);
  if (!setOutput(PIN_POWER_PULSE, true)) return false;
  delay(100);
  if (!setOutput(PIN_POWER_PULSE, false)) return false;
  delay(100);
  return setOutput(PIN_POWER_PULSE, true);
}
}  // namespace metalio
}  // namespace freeink
