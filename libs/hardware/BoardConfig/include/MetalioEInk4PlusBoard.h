#pragma once

// Metalio E-Ink4-Plus (ESP32-S31), TCA9555 at 0x20 on SDA0/SCL1. The same bus
// carries the TPS65185 panel PMIC (0x68), FT6336U touch, BQ27220, PCF8563 and
// SC7A20H. Expander indices 8..15 mean P1.0..P1.7, not ESP GPIO numbers.
// Pin roles and boot levels follow the vendor firmware
// (CloudZao/Metalio-E-INK4-Plus, main/boards/metalio-e-ink4-plus).
#include <Arduino.h>
#include <Wire.h>

#include <mutex>

#include "I2cBusLock.h"

namespace freeink {
namespace metalio_plus {
constexpr int SDA_PIN = 0;
constexpr int SCL_PIN = 1;
constexpr int EXPANDER_INT_GPIO = 2;
constexpr uint8_t EXPANDER_ADDR = 0x20;
constexpr uint8_t TPS_ADDR = 0x68;

constexpr uint8_t PIN_TPS_VCOM_CTRL = 0;   // P0.0
constexpr uint8_t PIN_TPS_PWRUP = 1;       // P0.1
constexpr uint8_t PIN_TPS_WAKEUP = 2;      // P0.2
constexpr uint8_t PIN_MOTOR = 3;           // P0.3, active high
constexpr uint8_t PIN_AMP_ENABLE = 4;      // P0.4, NS4150 PA_EN, active high
constexpr uint8_t PIN_SCREEN_POWER = 5;    // P0.5, camera/screen rail, active high
constexpr uint8_t PIN_TPS_PWR_GOOD = 7;    // P0.7, input
constexpr uint8_t PIN_VOLUME_UP = 8;       // P1.0, input, active low
constexpr uint8_t PIN_VOLUME_DOWN = 9;     // P1.1, input, active low
constexpr uint8_t PIN_TOUCH_RESET = 10;    // P1.2, high = powered (shared with 4G/camera reset)
constexpr uint8_t PIN_POWER_PULSE = 11;    // P1.3, shutdown pulse to the power IC, idles low
constexpr uint8_t PIN_BT_AMP_POWER = 12;   // P1.4, BT audio + amplifier rail, active high
constexpr uint8_t PIN_RTC_INT = 14;        // P1.6, input
constexpr uint8_t PIN_POWER_KEY = 15;      // P1.7, input, active low

constexpr uint16_t OUTPUTS = (1u << PIN_TPS_VCOM_CTRL) | (1u << PIN_TPS_PWRUP) | (1u << PIN_TPS_WAKEUP) |
                             (1u << PIN_MOTOR) | (1u << PIN_AMP_ENABLE) | (1u << PIN_SCREEN_POWER) |
                             (1u << PIN_TOUCH_RESET) | (1u << PIN_POWER_PULSE) | (1u << PIN_BT_AMP_POWER);

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

// Expander output latch, kept here so every caller (panel power, touch reset,
// motor, amplifier, power-off) updates its own bits without clobbering the rest.
inline uint16_t& outputLatch() {
  static uint16_t latch = 0;
  return latch;
}

inline std::mutex& outputMutex() {
  static std::mutex m;
  return m;
}

inline bool ready();

// Read/modify/write of the output latch. Call from tasks, never an ISR.
inline bool setOutputs(uint16_t mask, uint16_t levels) {
  if (!ready()) return false;
  std::lock_guard<std::mutex> lock(outputMutex());
  uint16_t& latch = outputLatch();
  latch = (latch & ~mask) | (levels & mask);
  return writeRegister(2, latch);
}

inline bool setOutput(uint8_t pin, bool high) {
  if (pin > 15) return false;
  const uint16_t mask = uint16_t(1) << pin;
  return setOutputs(mask, high ? mask : 0);
}

inline bool& readyFlag() {
  static bool r = false;
  return r;
}
inline bool ready() { return readyFlag(); }

// Brings up the shared I2C bus and the expander with the vendor's boot levels:
// TPS65185 control lines low (panel PMIC asleep), screen rail, touch reset and
// BT/amp rail high, power-off pulse, motor and amplifier low. Safe levels are
// latched before pins switch to outputs. waitMs bounds the retry window; callers
// passing 0 retry at most every RETRY_GAP_MS.
inline bool ensureBooted(uint32_t waitMs = 0) {
  constexpr unsigned long RETRY_GAP_MS = 500;
  static bool tried = false;
  static unsigned long lastFailure = 0;
  if (ready()) return true;
  freeink::enableI2cBusLock();
  if (waitMs == 0 && tried && millis() - lastFailure < RETRY_GAP_MS) return false;
  tried = true;
  uint16_t inputLevels = 0;
  const unsigned long start = millis();
  for (;;) {
    {
      const freeink::I2cBusLock busLock;
      if (Wire.begin(SDA_PIN, SCL_PIN, 400000)) {
        Wire.setTimeOut(10);
        if (readRegister(0, inputLevels)) break;
      }
      Wire.end();
    }
    if (millis() - start >= waitMs) {
      lastFailure = millis();
      return false;
    }
    delay(50);
  }
  constexpr uint16_t bootLevels = (1u << PIN_SCREEN_POWER) | (1u << PIN_TOUCH_RESET) | (1u << PIN_BT_AMP_POWER);
  {
    std::lock_guard<std::mutex> lock(outputMutex());
    outputLatch() = bootLevels;
    if (!writeRegister(2, bootLevels) || !writeRegister(4, 0) || !writeRegister(6, static_cast<uint16_t>(~OUTPUTS))) {
      return false;
    }
  }
  pinMode(EXPANDER_INT_GPIO, INPUT_PULLUP);
  readyFlag() = true;
  delay(80);  // vendor: let the rails settle before the panel PMIC is used
  return true;
}

// Input port levels. Isolated failed reads keep the last good sample; reads
// failing for longer than HOLD_MS release the keys.
inline uint16_t readInputs() {
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

// Fresh read of P0.7 PWR_GOOD for the panel power sequence (no sample cache).
inline bool tpsPowerGood() {
  uint16_t v;
  return ready() && readRegister(0, v) && (v & (1u << PIN_TPS_PWR_GOOD));
}

inline bool tpsWrite(uint8_t reg, uint8_t value) {
  const freeink::I2cBusLock busLock;
  Wire.beginTransmission(TPS_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

inline bool tpsRead(uint8_t reg, uint8_t& value) {
  const freeink::I2cBusLock busLock;
  Wire.beginTransmission(TPS_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;  // vendor: STOP between pointer write and read
  if (Wire.requestFrom(TPS_ADDR, uint8_t(1), uint8_t(true)) != 1) return false;
  value = Wire.read();
  return true;
}

inline bool tpsProbe() {
  const freeink::I2cBusLock busLock;
  Wire.beginTransmission(TPS_ADDR);
  return Wire.endTransmission() == 0;
}

inline bool setAmplifier(bool enabled) { return setOutput(PIN_AMP_ENABLE, enabled); }

// Blocking motor pulse (vendor default ~50 ms).
inline void vibrate(uint16_t durationMs = 50) {
  if (!setOutput(PIN_MOTOR, true)) return;
  delay(durationMs);
  setOutput(PIN_MOTOR, false);
}

// Touch controller reset through P1.2 (vendor: low >= 5 ms, then >= 300 ms).
inline void resetTouch() {
  if (!setOutput(PIN_TOUCH_RESET, false)) return;
  delay(10);
  setOutput(PIN_TOUCH_RESET, true);
  delay(300);
}

// Call only after display.deepSleep() and storage shutdown. The vendor powers
// off by toggling P1.3 (100 ms high, 100 ms low) until the power-switch chip
// cuts power. On battery this does not return; it returns once maxMs have
// passed with power still present (USB can hold it up), so the caller can fall
// back to deep sleep.
inline bool powerOff(uint32_t maxMs = 5000) {
  if (!ensureBooted()) return false;
  setAmplifier(false);
  setOutput(PIN_MOTOR, false);
  for (uint32_t elapsed = 0; elapsed < maxMs; elapsed += 200) {
    setOutput(PIN_POWER_PULSE, true);
    delay(100);
    setOutput(PIN_POWER_PULSE, false);
    delay(100);
  }
  return false;
}
}  // namespace metalio_plus
}  // namespace freeink
