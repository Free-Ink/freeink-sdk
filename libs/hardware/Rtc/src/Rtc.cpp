#include "Rtc.h"

#include <BoardConfig.h>

#if FREEINK_CAP_RTC

#include <Wire.h>
#include <soc/soc_caps.h>

namespace freeink {
namespace {

// PCF8563 register map (confirmed against the vendor peripheral demo).
constexpr uint8_t PCF8563_REG_CONTROL_STATUS1 = 0x00;
constexpr uint8_t PCF8563_REG_TIME = 0x02;  // seconds, minutes, hours, days, weekdays, months, years
constexpr uint8_t PCF8563_REG_CLKOUT = 0x0D;
constexpr uint8_t PCF8563_CLKOUT_DISABLED = 0x00;
constexpr uint8_t PCF8563_VL_FLAG = 0x80;  // seconds reg bit7: oscillator stopped / voltage-low

// DS3231 register map.
constexpr uint8_t DS3231_REG_TIME = 0x00;  // seconds, minutes, hours, day, date, month, year
constexpr uint8_t DS3231_REG_CONTROL = 0x0E;
constexpr uint8_t DS3231_REG_STATUS = 0x0F;
constexpr uint8_t DS3231_CONTROL_INTCN = 0x04;
constexpr uint8_t DS3231_STATUS_OSF = 0x80;

bool g_wireReady[2] = {false, false};
TwoWire& sensorWire() {
  const auto& s = BoardConfig::ACTIVE.sensors;
#if SOC_I2C_NUM > 1
  return s.i2cBus == 1 ? Wire1 : Wire;
#else
  return Wire;
#endif
}

void ensureWire() {
  const auto& s = BoardConfig::ACTIVE.sensors;
  const uint8_t bus =
#if SOC_I2C_NUM > 1
      s.i2cBus == 1 ? 1 : 0;
#else
      0;
#endif
  if (g_wireReady[bus]) return;
  auto& wire = sensorWire();
  wire.begin(s.i2cSda, s.i2cScl, s.i2cHz);
  g_wireReady[bus] = true;
}

uint8_t bcdToDec(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10U + (v & 0x0FU)); }
uint8_t decToBcd(uint8_t v) { return static_cast<uint8_t>((v / 10U) << 4 | (v % 10U)); }

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  ensureWire();
  auto& wire = sensorWire();
  wire.beginTransmission(addr);
  wire.write(reg);
  wire.write(value);
  return wire.endTransmission() == 0;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* dst, uint8_t len) {
  ensureWire();
  auto& wire = sensorWire();
  wire.beginTransmission(addr);
  wire.write(reg);
  if (wire.endTransmission(false) != 0) return false;
  if (wire.requestFrom(addr, len, static_cast<uint8_t>(true)) < len) return false;
  for (uint8_t i = 0; i < len; ++i) dst[i] = wire.read();
  return true;
}

}  // namespace

bool Rtc::begin() {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  if (addr == 0) return false;
  const auto& s = BoardConfig::ACTIVE.sensors;
  if (s.i2cSda < 0 || s.i2cScl < 0 || s.i2cHz == 0) return false;
  ensureWire();
  uint8_t status = 0;
  switch (s.rtcType) {
    case BoardConfig::RtcType::Pcf8563:
      if (!readRegs(addr, PCF8563_REG_CONTROL_STATUS1, &status, 1)) return false;
      writeReg(addr, PCF8563_REG_CLKOUT, PCF8563_CLKOUT_DISABLED);  // we don't use the 32 kHz CLKOUT
      break;
    case BoardConfig::RtcType::Ds3231:
      if (!readRegs(addr, DS3231_REG_STATUS, &status, 1)) return false;
      writeReg(addr, DS3231_REG_CONTROL, DS3231_CONTROL_INTCN);  // disable square-wave output
      break;
    case BoardConfig::RtcType::None:
      return false;
  }
  begun_ = true;
  return true;
}

bool Rtc::now(DateTime& out) {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  if (!begun_ || addr == 0) return false;
  const auto& s = BoardConfig::ACTIVE.sensors;
  uint8_t raw[7] = {};
  switch (s.rtcType) {
    case BoardConfig::RtcType::Pcf8563: {
      if (!readRegs(addr, PCF8563_REG_TIME, raw, sizeof(raw))) return false;
      if (raw[0] & PCF8563_VL_FLAG) return false;  // oscillator stopped -> time not trustworthy
      out.second = bcdToDec(raw[0] & 0x7FU);
      out.minute = bcdToDec(raw[1] & 0x7FU);
      out.hour = bcdToDec(raw[2] & 0x3FU);
      out.day = bcdToDec(raw[3] & 0x3FU);
      out.weekday = bcdToDec(raw[4] & 0x07U);
      out.month = bcdToDec(raw[5] & 0x1FU);
      const uint8_t yy = bcdToDec(raw[6]);
      out.year = (raw[5] & 0x80U) ? static_cast<uint16_t>(1900 + yy) : static_cast<uint16_t>(2000 + yy);
      return true;
    }
    case BoardConfig::RtcType::Ds3231: {
      uint8_t status = 0;
      if (!readRegs(addr, DS3231_REG_STATUS, &status, 1) || (status & DS3231_STATUS_OSF)) return false;
      if (!readRegs(addr, DS3231_REG_TIME, raw, sizeof(raw))) return false;
      out.second = bcdToDec(raw[0] & 0x7FU);
      out.minute = bcdToDec(raw[1] & 0x7FU);
      if (raw[2] & 0x40U) {
        uint8_t h12 = bcdToDec(raw[2] & 0x1FU);
        if (h12 == 12) h12 = 0;
        out.hour = static_cast<uint8_t>((raw[2] & 0x20U) ? h12 + 12 : h12);
      } else {
        out.hour = bcdToDec(raw[2] & 0x3FU);
      }
      out.weekday = bcdToDec(raw[3] & 0x07U) % 7U;
      out.day = bcdToDec(raw[4] & 0x3FU);
      out.month = bcdToDec(raw[5] & 0x1FU);
      out.year = static_cast<uint16_t>(2000 + bcdToDec(raw[6]));
      return true;
    }
    case BoardConfig::RtcType::None:
      return false;
  }
  return false;
}

bool Rtc::set(const DateTime& dt) {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  if (!begun_ || addr == 0) return false;
  const auto& s = BoardConfig::ACTIVE.sensors;
  const uint8_t centuryBit = dt.year < 2000 ? 0x80U : 0x00U;
  ensureWire();
  auto& wire = sensorWire();
  if (s.rtcType == BoardConfig::RtcType::None) return false;
  wire.beginTransmission(addr);
  wire.write(s.rtcType == BoardConfig::RtcType::Pcf8563 ? PCF8563_REG_TIME : DS3231_REG_TIME);
  wire.write(decToBcd(dt.second));  // also clears VL once a valid time is written
  wire.write(decToBcd(dt.minute));
  wire.write(decToBcd(dt.hour));
  if (s.rtcType == BoardConfig::RtcType::Pcf8563) {
    wire.write(decToBcd(dt.day));
    wire.write(decToBcd(dt.weekday));
    wire.write(static_cast<uint8_t>(decToBcd(dt.month) | centuryBit));
  } else {
    wire.write(decToBcd(dt.weekday == 0 ? 7 : dt.weekday));
    wire.write(decToBcd(dt.day));
    wire.write(decToBcd(dt.month));
  }
  wire.write(decToBcd(static_cast<uint8_t>(dt.year % 100)));
  if (wire.endTransmission() != 0) return false;
  if (s.rtcType == BoardConfig::RtcType::Ds3231) {
    uint8_t status = 0;
    if (readRegs(addr, DS3231_REG_STATUS, &status, 1)) {
      writeReg(addr, DS3231_REG_STATUS, static_cast<uint8_t>(status & ~DS3231_STATUS_OSF));
    }
  }
  return true;
}

}  // namespace freeink

#else  // FREEINK_CAP_RTC — no RTC on this board.

namespace freeink {
bool Rtc::begin() { return false; }
bool Rtc::now(DateTime&) { return false; }
bool Rtc::set(const DateTime&) { return false; }
}  // namespace freeink

#endif  // FREEINK_CAP_RTC
