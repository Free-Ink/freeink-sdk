#pragma once

// X-Powers AXP2101 PMIC — board support for the Waveshare ESP32-S3-ePaper-3.97.
//
// On that board the PMIC is not an optional extra: ALDO3 is the e-paper rail
// (the panel is dead until it is enabled), and the PMIC's fuel-gauge block is
// the only battery telemetry — there is no ADC divider and no gauge chip. Two
// SDK modules therefore talk to it (EpdBus for the rail, BatteryMonitor for the
// gauge), so the register map and the bus live here, once.
//
// Register semantics follow the AXP2101 datasheet, cross-checked against the
// vendor demo's XPowersLib usage (components/axpPower in
// waveshareteam/ESP32-S3-ePaper-3.97).
//
// Header-only inline I2C, same pattern as M5Pm1.h: both callers already depend
// on BoardConfig, so no extra lib wiring.

#include <Arduino.h>
#include <Wire.h>

#include "BoardConfig.h"

namespace freeink {
namespace axp2101 {

// --- registers ---
constexpr uint8_t REG_STATUS1 = 0x00;        // [5] VBUS good, [3] battery present
constexpr uint8_t REG_STATUS2 = 0x01;        // [7:5] charge state (001 = charging)
constexpr uint8_t REG_COMMON_CONFIG = 0x10;  // [0] soft power-off
constexpr uint8_t REG_ADC_CHANNEL_CTRL = 0x30;  // [0] VBAT ADC enable
constexpr uint8_t REG_VBAT_H = 0x34;            // 13-bit VBAT in mV, H5L8 over 0x34/0x35
constexpr uint8_t REG_LDO_ONOFF_CTRL0 = 0x90;   // [2] ALDO3 enable
constexpr uint8_t REG_ALDO3_VOL = 0x94;         // [4:0] (mV - 500) / 100
constexpr uint8_t REG_FUEL_GAUGE_CTRL = 0xA2;   // [0] gauge enable
constexpr uint8_t REG_BAT_PERCENT = 0xA4;       // SoC, whole percent

constexpr uint8_t ALDO3_BIT = 1 << 2;
constexpr uint8_t GAUGE_EN_BIT = 1 << 0;
constexpr uint8_t VBAT_ADC_EN_BIT = 1 << 0;
constexpr uint8_t POWEROFF_BIT = 1 << 0;
constexpr uint8_t STATUS1_VBUS_GOOD = 1 << 5;
constexpr uint8_t ALDO3_VOL_3V3 = (3300 - 500) / 100;  // REG_ALDO3_VOL code for 3.3 V

namespace detail {

inline uint8_t address() { return BoardConfig::ACTIVE.batteryGauge.gaugeAddr; }

// The PMIC shares the board's only I2C bus with the RTC/IMU, so the pins come
// from the profile rather than a private define. begin() is idempotent in the
// Arduino core; other users of the bus (Rtc, Imu) call it with the same values.
inline void beginBus() {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  static bool started = false;
  if (started) return;
  started = true;
  Wire.begin(g.i2cSda, g.i2cScl, g.i2cHz);
}

inline bool readReg(uint8_t reg, uint8_t& out) {
  beginBus();
  Wire.beginTransmission(address());
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address(), static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  out = Wire.read();
  return true;
}

inline bool writeReg(uint8_t reg, uint8_t value) {
  beginBus();
  Wire.beginTransmission(address());
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

inline bool updateReg(uint8_t reg, uint8_t clearMask, uint8_t setMask) {
  uint8_t value = 0;
  if (!readReg(reg, value)) return false;
  const uint8_t updated = static_cast<uint8_t>((value & ~clearMask) | setMask);
  if (updated == value) return true;
  return writeReg(reg, updated);
}

}  // namespace detail

// One-time bring-up (idempotent, cheap after the first call): battery voltage
// ADC and the fuel gauge on, ALDO3 pinned to 3.3 V. Rail enable itself is left
// to setEpdPower() so a board that sleeps with the panel off stays off.
inline bool ensureBooted() {
  static bool booted = false;
  static bool ok = false;
  if (booted) return ok;
  booted = true;
  if (detail::address() == 0) return false;
  ok = detail::updateReg(REG_ADC_CHANNEL_CTRL, 0, VBAT_ADC_EN_BIT) &&
       detail::updateReg(REG_FUEL_GAUGE_CTRL, 0, GAUGE_EN_BIT) &&
       detail::updateReg(REG_ALDO3_VOL, 0x1F, ALDO3_VOL_3V3);
  return ok;
}

// EPD rail (ALDO3). EpdBus's own 100 ms settle covers power-on.
inline void setEpdPower(bool enabled) {
  ensureBooted();
  detail::updateReg(REG_LDO_ONOFF_CTRL0, enabled ? 0 : ALDO3_BIT, enabled ? ALDO3_BIT : 0);
}

// SoC in whole percent. false when the PMIC does not answer.
inline bool batteryPercent(uint8_t& out) {
  if (!ensureBooted()) return false;
  uint8_t soc = 0;
  if (!detail::readReg(REG_BAT_PERCENT, soc)) return false;
  if (soc > 100) return false;  // 0xFF while the gauge is still settling
  out = soc;
  return true;
}

inline bool batteryMillivolts(uint16_t& out) {
  if (!ensureBooted()) return false;
  uint8_t hi = 0, lo = 0;
  if (!detail::readReg(REG_VBAT_H, hi) || !detail::readReg(static_cast<uint8_t>(REG_VBAT_H + 1), lo)) return false;
  out = static_cast<uint16_t>((hi & 0x1F) << 8) | lo;  // already millivolts
  return true;
}

// Charge state from STATUS2[7:5]; 001 = charging.
inline bool isCharging(bool& known) {
  uint8_t status = 0;
  if (!ensureBooted() || !detail::readReg(REG_STATUS2, status)) {
    known = false;
    return false;
  }
  known = true;
  return ((status >> 5) & 0x07) == 0x01;
}

inline bool isVbusPresent() {
  uint8_t status = 0;
  if (!ensureBooted() || !detail::readReg(REG_STATUS1, status)) return false;
  return (status & STATUS1_VBUS_GOOD) != 0;
}

// Full power-off (the PMIC drops every rail). A PWRKEY press boots the board again.
inline bool shutdown() {
  if (!ensureBooted()) return false;
  return detail::updateReg(REG_COMMON_CONFIG, 0, POWEROFF_BIT);
}

}  // namespace axp2101
}  // namespace freeink
