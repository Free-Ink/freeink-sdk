#pragma once

// Metalio E-Ink 4, TCA9555 at 0x20 on SDA41/SCL42.
// Expander indices 8..15 mean P1.0..P1.7, not ESP GPIO numbers.
#include <Arduino.h>
#include <Wire.h>
#include <mutex>
#include "I2cBusLock.h"

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
constexpr int SD_DAT3_GPIO = 46;  // input-only; held high even with 1-bit SDMMC

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

// Read/modify/write preserves unrelated expander outputs. Call board functions
// from the firmware's hardware task, never an ISR.
inline bool setOutput(uint8_t pin, bool high) {
  if (pin > 15) return false;
  static std::mutex outputMutex;
  std::lock_guard<std::mutex> lock(outputMutex);
  uint16_t value;
  if (!readRegister(2, value)) return false;
  const uint16_t mask = uint16_t(1) << pin;
  return writeRegister(2, high ? value | mask : value & ~mask);
}

// A device reset mid-transfer can hold SDA low; clock it free and send STOP. Call with the
// bus stopped (Wire.end()), since it drives the pins directly.
inline void clearBus() {
  constexpr int SDA_PIN = 41, SCL_PIN = 42;
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(5);
  for (int i = 0; i < 9 && digitalRead(SDA_PIN) == LOW; ++i) {
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(5);
  }
  pinMode(SDA_PIN, OUTPUT_OPEN_DRAIN);  // STOP: SDA rises while SCL is high
  digitalWrite(SDA_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(SDA_PIN, HIGH);
  delayMicroseconds(5);
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
}

// On some boards the I2C bus cannot start until ~2 s after power-up, so each failed attempt
// restarts the bus. waitMs bounds how long this call retries; callers passing 0 retry at most
// every RETRY_GAP_MS, so a missing expander does not stall every poll.
inline bool ensureBooted(uint32_t waitMs = 0) {
  constexpr unsigned long RETRY_GAP_MS = 500;
  static bool ready = false;
  static bool tried = false;
  static unsigned long lastFailure = 0;
  if (ready) return true;
  freeink::enableI2cBusLock();  // first call runs in setup(), before the render task exists
  if (waitMs == 0 && tried && millis() - lastFailure < RETRY_GAP_MS) return false;
  tried = true;
  constexpr uint16_t outputs = (1u << PIN_AMP_SELECT) | (1u << PIN_AMP_ENABLE) |
      (1u << PIN_SCREEN_SD_POWER) | (1u << PIN_MAIN_POWER) |
      (1u << PIN_TOUCH_RESET) | (1u << PIN_POWER_PULSE);
  constexpr uint16_t inputs = (1u << PIN_VOLUME_DOWN) | (1u << PIN_VOLUME_UP) | (1u << 12);
  uint16_t latch, config, polarity;
  const unsigned long start = millis();
  for (;;) {
    {
      const freeink::I2cBusLock busLock;  // begin/end must not race a transfer
      if (Wire.begin(41, 42, 400000)) {
        Wire.setTimeOut(10);
        if (readRegister(2, latch) && readRegister(6, config) && readRegister(4, polarity)) break;
      }
      Wire.end();
      clearBus();
    }
    if (millis() - start >= waitMs) {
      lastFailure = millis();
      return false;
    }
    delay(50);
  }
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

// Isolated failed reads keep the last good sample, as the vendor firmware does. Reads failing
// for longer than HOLD_MS release the keys instead of retaining a stale pressed state.
inline uint16_t readButtons() {
  constexpr unsigned long HOLD_MS = 100;
  static unsigned long sampledAt = 0;
  static unsigned long goodAt = 0;
  static uint16_t value = 0xFFFF;
  static bool sampled = false;
  const unsigned long now = millis();
  if (sampled && now - sampledAt < 20) return value;
  sampled = true;
  sampledAt = now;
  uint16_t fresh;
  if (ensureBooted() && readRegister(0, fresh)) {
    value = fresh;
    goodAt = now;
  } else if (now - goodAt > HOLD_MS) {
    value = 0xFFFF;
  }
  return value;
}

inline bool setAmplifier(bool enabled) {
  return ensureBooted() && setOutput(PIN_AMP_SELECT, false) && setOutput(PIN_AMP_ENABLE, enabled);
}

// Legacy blocking GPIO pulse. Prefer HapticManager for new consumers.
// Do not mix this helper with an attached HapticManager PWM output.
inline void vibrate(uint16_t durationMs = 35) {
  if (!ensureBooted()) return;
  digitalWrite(VIBRATION_GPIO, HIGH);
  delay(durationMs);
  digitalWrite(VIBRATION_GPIO, LOW);
}

// Call only after display.deepSleep() and storage shutdown. The vendor firmware's sequence
// (power_hw.cc PwrOffTask): amplifier off, main and screen/card rails held on, then the power key
// toggled every 100 ms until the power-switch chip cuts power. A few spaced pulses do not switch
// it off. On battery this does not return; it returns once maxMs have passed with power still
// present (USB can hold it up), so the caller can fall back to deep sleep.
inline bool powerOff(uint32_t maxMs = 5000) {
  if (!ensureBooted() || !setAmplifier(false)) return false;
  if (!setOutput(PIN_MAIN_POWER, true) || !setOutput(PIN_SCREEN_SD_POWER, true)) return false;
  for (uint32_t elapsed = 0; elapsed < maxMs; elapsed += 200) {
    setOutput(PIN_POWER_PULSE, true);
    delay(100);
    setOutput(PIN_POWER_PULSE, false);
    delay(100);
  }
  return setOutput(PIN_POWER_PULSE, true);
}
}  // namespace metalio
}  // namespace freeink
