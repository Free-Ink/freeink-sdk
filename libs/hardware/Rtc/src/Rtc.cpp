#include "Rtc.h"

#include <BoardConfig.h>

#if FREEINK_CAP_RTC

#include <Wire.h>

namespace freeink {
namespace {

// PCF8563 register map (confirmed against the vendor peripheral demo).
constexpr uint8_t REG_CONTROL_STATUS1 = 0x00;
constexpr uint8_t REG_TIME = 0x02;  // seconds, minutes, hours, days, weekdays, months, years
constexpr uint8_t REG_CLKOUT = 0x0D;
constexpr uint8_t CLKOUT_DISABLED = 0x00;
constexpr uint8_t VL_FLAG = 0x80;  // seconds reg bit7: oscillator stopped / voltage-low

bool g_wireReady = false;
void ensureWire() {
  if (g_wireReady) return;
  const auto& s = BoardConfig::ACTIVE.sensors;
  Wire.begin(s.i2cSda, s.i2cScl, s.i2cHz);
  g_wireReady = true;
}

uint8_t bcdToDec(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10U + (v & 0x0FU)); }
uint8_t decToBcd(uint8_t v) { return static_cast<uint8_t>((v / 10U) << 4 | (v % 10U)); }

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  ensureWire();
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* dst, uint8_t len) {
  ensureWire();
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, len, static_cast<uint8_t>(true)) < len) return false;
  for (uint8_t i = 0; i < len; ++i) dst[i] = Wire.read();
  return true;
}

}  // namespace

bool Rtc::begin() {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  if (addr == 0) return false;
  ensureWire();
  // Probe: a bus read of control/status 1 must ACK.
  uint8_t status;
  if (!readRegs(addr, REG_CONTROL_STATUS1, &status, 1)) return false;
  writeReg(addr, REG_CLKOUT, CLKOUT_DISABLED);  // we don't use the 32 kHz CLKOUT
  begun_ = true;
  return true;
}

bool Rtc::now(DateTime& out) {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  if (!begun_ || addr == 0) return false;
  uint8_t raw[7];
  if (!readRegs(addr, REG_TIME, raw, sizeof(raw))) return false;
  if (raw[0] & VL_FLAG) return false;  // oscillator stopped -> time not trustworthy

  out.second = bcdToDec(raw[0] & 0x7FU);
  out.minute = bcdToDec(raw[1] & 0x7FU);
  out.hour = bcdToDec(raw[2] & 0x3FU);
  out.day = bcdToDec(raw[3] & 0x3FU);
  out.weekday = bcdToDec(raw[4] & 0x07U);
  out.month = bcdToDec(raw[5] & 0x1FU);
  // Century bit (months reg bit7): set => 1900s, clear => 2000s (PCF8563 convention).
  const uint8_t yy = bcdToDec(raw[6]);
  out.year = (raw[5] & 0x80U) ? static_cast<uint16_t>(1900 + yy) : static_cast<uint16_t>(2000 + yy);
  return true;
}

bool Rtc::set(const DateTime& dt) {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  if (!begun_ || addr == 0) return false;
  const uint8_t centuryBit = dt.year < 2000 ? 0x80U : 0x00U;
  ensureWire();
  Wire.beginTransmission(addr);
  Wire.write(REG_TIME);
  Wire.write(decToBcd(dt.second));  // also clears VL once a valid time is written
  Wire.write(decToBcd(dt.minute));
  Wire.write(decToBcd(dt.hour));
  Wire.write(decToBcd(dt.day));
  Wire.write(decToBcd(dt.weekday));
  Wire.write(static_cast<uint8_t>(decToBcd(dt.month) | centuryBit));
  Wire.write(decToBcd(static_cast<uint8_t>(dt.year % 100)));
  return Wire.endTransmission() == 0;
}

}  // namespace freeink

#else  // FREEINK_CAP_RTC — no RTC on this board.

namespace freeink {
bool Rtc::begin() { return false; }
bool Rtc::now(DateTime&) { return false; }
bool Rtc::set(const DateTime&) { return false; }
}  // namespace freeink

#endif  // FREEINK_CAP_RTC
