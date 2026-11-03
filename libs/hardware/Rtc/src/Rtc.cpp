#include "Rtc.h"

#include <BoardConfig.h>
#include <time.h>

#if FREEINK_CAP_RTC

#include <Wire.h>
#include <I2cBusLock.h>
#include <soc/soc_caps.h>

namespace freeink {
namespace {

// PCF8563 register map (confirmed against the vendor peripheral demo).
constexpr uint8_t PCF8563_REG_CONTROL_STATUS1 = 0x00;
constexpr uint8_t PCF8563_REG_TIME = 0x02;  // seconds, minutes, hours, days, weekdays, months, years
constexpr uint8_t PCF8563_REG_CLKOUT = 0x0D;
constexpr uint8_t PCF8563_CLKOUT_DISABLED = 0x00;
constexpr uint8_t PCF8563_VL_FLAG = 0x80;  // seconds reg bit7: oscillator stopped / voltage-low

// PCF85063A register map. Same BCD time layout as the PCF8563 but shifted:
// Control_1/2 at 0x00/0x01, time from 0x04, seconds bit7 is the OS (oscillator
// stopped) flag, and Months carries no century bit.
constexpr uint8_t PCF85063_REG_CONTROL1 = 0x00;
constexpr uint8_t PCF85063_REG_TIME = 0x04;
constexpr uint8_t PCF85063_OS_FLAG = 0x80;

// DS3231 register map.
constexpr uint8_t DS3231_REG_TIME = 0x00;  // seconds, minutes, hours, day, date, month, year
constexpr uint8_t DS3231_REG_CONTROL = 0x0E;
constexpr uint8_t DS3231_REG_STATUS = 0x0F;
constexpr uint8_t DS3231_CONTROL_INTCN = 0x04;
constexpr uint8_t DS3231_STATUS_OSF = 0x80;

// RX8130CE register map (Paper Mono / PaperS3).
constexpr uint8_t RX8130_REG_TIME = 0x10;
constexpr uint8_t RX8130_REG_COUNTER = 0x1A;
constexpr uint8_t RX8130_REG_EXTENSION = 0x1C;
constexpr uint8_t RX8130_REG_FLAGS = 0x1D;
constexpr uint8_t RX8130_REG_CONTROL0 = 0x1E;
constexpr uint8_t RX8130_VLF = 0x02;
constexpr uint8_t RX8130_STOP = 0x40;
constexpr uint8_t RX8130_TIMER = 0x10;  // TE, TF and TIE occupy bit 4.
constexpr uint8_t RX8130_OTHER_IRQS = 0x28;
constexpr uint8_t RX8130_TEST = 0x80;
constexpr uint8_t RX8130_TIMER_STOP = 0x04;
constexpr uint8_t RX8130_BACKUP_ONLY = 0x01;
constexpr uint8_t RX8130_CLEAR_VLF = 0xBD;
constexpr uint8_t RX8130_CLEAR_TF = 0xAF;  // W0C: preserve the other flags.

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
  const freeink::I2cBusLock busLock;
  ensureWire();
  auto& wire = sensorWire();
  wire.beginTransmission(addr);
  wire.write(reg);
  wire.write(value);
  return wire.endTransmission() == 0;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* dst, uint8_t len) {
  const freeink::I2cBusLock busLock;
  ensureWire();
  auto& wire = sensorWire();
  wire.beginTransmission(addr);
  wire.write(reg);
  if (wire.endTransmission(false) != 0) return false;
  if (wire.requestFrom(addr, len, static_cast<uint8_t>(true)) < len) return false;
  for (uint8_t i = 0; i < len; ++i) dst[i] = wire.read();
  return true;
}

// Presence read for begin(). A single failed transaction is not proof of a
// missing chip: on a shared bus the I2C driver can fail the first transfer after
// another device's NACK (e.g. an IMU address probe) with ESP_ERR_INVALID_STATE.
bool readRegsRetry(uint8_t addr, uint8_t reg, uint8_t* dst, uint8_t len) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (readRegs(addr, reg, dst, len)) return true;
    delay(10);
  }
  return false;
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
      if (!readRegsRetry(addr, PCF8563_REG_CONTROL_STATUS1, &status, 1)) return false;
      writeReg(addr, PCF8563_REG_CLKOUT, PCF8563_CLKOUT_DISABLED);  // we don't use the 32 kHz CLKOUT
      break;
    case BoardConfig::RtcType::Pcf85063:
      // No CLKOUT write: the PCF85063's CLKOUT lives in Control_2 alongside bits
      // this driver has no business touching, and it boots disabled.
      if (!readRegsRetry(addr, PCF85063_REG_CONTROL1, &status, 1)) return false;
      break;
    case BoardConfig::RtcType::Ds3231:
      if (!readRegsRetry(addr, DS3231_REG_STATUS, &status, 1)) return false;
      writeReg(addr, DS3231_REG_CONTROL, DS3231_CONTROL_INTCN);  // disable square-wave output
      break;
    case BoardConfig::RtcType::Rx8130:
      if (!readRegsRetry(addr, RX8130_REG_CONTROL0, &status, 1)) return false;
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
    case BoardConfig::RtcType::Pcf85063: {
      if (!readRegs(addr, PCF85063_REG_TIME, raw, sizeof(raw))) return false;
      if (raw[0] & PCF85063_OS_FLAG) return false;  // oscillator stopped -> time not trustworthy
      out.second = bcdToDec(raw[0] & 0x7FU);
      out.minute = bcdToDec(raw[1] & 0x7FU);
      out.hour = bcdToDec(raw[2] & 0x3FU);
      out.day = bcdToDec(raw[3] & 0x3FU);
      out.weekday = bcdToDec(raw[4] & 0x07U);
      out.month = bcdToDec(raw[5] & 0x1FU);
      out.year = static_cast<uint16_t>(2000 + bcdToDec(raw[6]));
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
    case BoardConfig::RtcType::Rx8130: {
      uint8_t flags = 0;
      if (!readRegs(addr, RX8130_REG_FLAGS, &flags, 1) || (flags & RX8130_VLF)) return false;
      if (!readRegs(addr, RX8130_REG_TIME, raw, sizeof(raw))) return false;
      out.second = bcdToDec(raw[0] & 0x7FU);
      out.minute = bcdToDec(raw[1] & 0x7FU);
      out.hour = bcdToDec(raw[2] & 0x3FU);
      out.weekday = 0;
      for (uint8_t i = 0; i < 7; ++i) {
        if (raw[3] & (1u << i)) {
          out.weekday = i;
          break;
        }
      }
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
  const freeink::I2cBusLock busLock;  // direct transfers below, not only helpers
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  if (!begun_ || addr == 0) return false;
  const auto& s = BoardConfig::ACTIVE.sensors;
  const uint8_t centuryBit = dt.year < 2000 ? 0x80U : 0x00U;
  ensureWire();
  auto& wire = sensorWire();
  if (s.rtcType == BoardConfig::RtcType::None) return false;
  if (s.rtcType == BoardConfig::RtcType::Rx8130) {
    uint8_t control = 0;
    if (!readRegs(addr, RX8130_REG_CONTROL0, &control, 1) ||
        !writeReg(addr, RX8130_REG_CONTROL0, static_cast<uint8_t>(control | RX8130_STOP))) {
      return false;
    }
    wire.beginTransmission(addr);
    wire.write(RX8130_REG_TIME);
    wire.write(decToBcd(dt.second));
    wire.write(decToBcd(dt.minute));
    wire.write(decToBcd(dt.hour));
    wire.write(static_cast<uint8_t>(1u << (dt.weekday % 7u)));
    wire.write(decToBcd(dt.day));
    wire.write(decToBcd(dt.month));
    wire.write(decToBcd(static_cast<uint8_t>(dt.year % 100)));
    const bool written = wire.endTransmission() == 0;
    const bool restarted = writeReg(addr, RX8130_REG_CONTROL0, static_cast<uint8_t>(control & ~RX8130_STOP));
    return written && restarted &&
           writeReg(addr, RX8130_REG_FLAGS, RX8130_CLEAR_VLF);  // W0C: clear VLF after setting time.
  }
  if (s.rtcType == BoardConfig::RtcType::Pcf85063) {
    wire.beginTransmission(addr);
    wire.write(PCF85063_REG_TIME);
    wire.write(decToBcd(dt.second));  // also clears OS once a valid time is written
    wire.write(decToBcd(dt.minute));
    wire.write(decToBcd(dt.hour));
    wire.write(decToBcd(dt.day));
    wire.write(decToBcd(dt.weekday));
    wire.write(decToBcd(dt.month));
    wire.write(decToBcd(static_cast<uint8_t>(dt.year % 100)));
    return wire.endTransmission() == 0;
  }
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

bool Rtc::cancelWakeTimer() {
  if (!begun_ || BoardConfig::ACTIVE.sensors.rtcType != BoardConfig::RtcType::Rx8130) return false;
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  uint8_t extension = 0, control = 0;
  bool stopped = readRegs(addr, RX8130_REG_EXTENSION, &extension, 1);
  if (stopped) stopped = writeReg(addr, RX8130_REG_EXTENSION, extension & ~RX8130_TIMER);
  bool disabled = readRegs(addr, RX8130_REG_CONTROL0, &control, 1);
  if (disabled) disabled = writeReg(addr, RX8130_REG_CONTROL0, control & ~RX8130_TIMER);
  const bool cleared = writeReg(addr, RX8130_REG_FLAGS, RX8130_CLEAR_TF);
  const bool readExtension = readRegs(addr, RX8130_REG_EXTENSION, &extension, 1);
  const bool readControl = readRegs(addr, RX8130_REG_CONTROL0, &control, 1);
  return stopped && disabled && cleared && readExtension && readControl && !(extension & RX8130_TIMER) &&
         !(control & RX8130_TIMER);
}

bool Rtc::armWakeTimer(const uint32_t seconds) {
  const freeink::I2cBusLock busLock;  // direct transfers below, not only helpers
  if (!begun_ || BoardConfig::ACTIVE.sensors.rtcType != BoardConfig::RtcType::Rx8130 || seconds == 0 ||
      seconds > 65535UL * 60UL)
    return false;
  if (!cancelWakeTimer()) return false;
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  uint8_t extension = 0, control = 0, flags = 0;
  if (!readRegs(addr, RX8130_REG_EXTENSION, &extension, 1) || !readRegs(addr, RX8130_REG_CONTROL0, &control, 1) ||
      !readRegs(addr, RX8130_REG_FLAGS, &flags, 1))
    return false;
  // Do not borrow a stopped clock or replace another interrupt owner.
  if ((control & (RX8130_TEST | RX8130_STOP | RX8130_OTHER_IRQS)) || (flags & RX8130_VLF)) return false;
  const bool minutes = seconds > 65535UL;
  const uint16_t count = static_cast<uint16_t>(minutes ? (seconds + 59UL) / 60UL : seconds);
  extension = static_cast<uint8_t>((extension & ~0x17U) | (minutes ? 0x03U : 0x02U));
  // Count in both main and backup supply modes; the caller consumes each event.
  control = static_cast<uint8_t>((control & ~(RX8130_TIMER_STOP | RX8130_BACKUP_ONLY)) | RX8130_TIMER);
  bool ok = writeReg(addr, RX8130_REG_EXTENSION, extension) && writeReg(addr, RX8130_REG_FLAGS, RX8130_CLEAR_TF) &&
            writeReg(addr, RX8130_REG_CONTROL0, control);
  if (ok) {
    auto& wire = sensorWire();
    wire.beginTransmission(addr);
    wire.write(RX8130_REG_COUNTER);
    wire.write(static_cast<uint8_t>(count));
    wire.write(static_cast<uint8_t>(count >> 8));
    ok = wire.endTransmission() == 0;
    uint8_t readback[2] = {};
    ok = ok && readRegs(addr, RX8130_REG_COUNTER, readback, sizeof(readback)) &&
         readback[0] == static_cast<uint8_t>(count) && readback[1] == static_cast<uint8_t>(count >> 8);
  }
  if (ok) ok = writeReg(addr, RX8130_REG_EXTENSION, extension | RX8130_TIMER);
  uint8_t finalExtension = 0, finalControl = 0;
  ok = ok && readRegs(addr, RX8130_REG_EXTENSION, &finalExtension, 1) &&
       readRegs(addr, RX8130_REG_CONTROL0, &finalControl, 1) &&
       finalExtension == static_cast<uint8_t>(extension | RX8130_TIMER) && finalControl == control;
  if (!ok) cancelWakeTimer();  // Caller must also disarm the external wake route on failure.
  return ok;
}

bool Rtc::readWakeTimer(bool& fired) {
  fired = false;
  if (!begun_ || BoardConfig::ACTIVE.sensors.rtcType != BoardConfig::RtcType::Rx8130) return false;
  const uint8_t addr = BoardConfig::ACTIVE.sensors.rtcAddr;
  uint8_t flags = 0, extension = 0, control = 0;
  if (!readRegs(addr, RX8130_REG_FLAGS, &flags, 1) || !readRegs(addr, RX8130_REG_EXTENSION, &extension, 1) ||
      !readRegs(addr, RX8130_REG_CONTROL0, &control, 1))
    return false;
  fired = (flags & (RX8130_TIMER | RX8130_VLF)) == RX8130_TIMER && (extension & RX8130_TIMER) &&
          (control & (RX8130_TEST | RX8130_STOP | RX8130_OTHER_IRQS | RX8130_TIMER)) == RX8130_TIMER;
  return true;
}

bool Rtc::adjust(const int32_t seconds, DateTime* out) {
  DateTime now{};
  if (!this->now(now)) return false;
  // Round-trip through mktime so day/month/year carries are calendar-correct
  // (mktime normalizes out-of-range fields; the fixed offset cancels).
  struct tm t{};
  t.tm_year = now.year - 1900;
  t.tm_mon = now.month - 1;
  t.tm_mday = now.day;
  t.tm_hour = now.hour;
  t.tm_min = now.minute;
  t.tm_sec = now.second;
  time_t epoch = mktime(&t) + seconds;
  localtime_r(&epoch, &t);
  const DateTime dt{static_cast<uint16_t>(t.tm_year + 1900), static_cast<uint8_t>(t.tm_mon + 1),
                    static_cast<uint8_t>(t.tm_mday),         static_cast<uint8_t>(t.tm_hour),
                    static_cast<uint8_t>(t.tm_min),          static_cast<uint8_t>(t.tm_sec),
                    static_cast<uint8_t>(t.tm_wday)};
  if (!set(dt)) return false;
  if (out) *out = dt;
  return true;
}

}  // namespace freeink

#else  // FREEINK_CAP_RTC — no RTC on this board.

namespace freeink {
bool Rtc::begin() { return false; }
bool Rtc::now(DateTime&) { return false; }
bool Rtc::set(const DateTime&) { return false; }
bool Rtc::armWakeTimer(uint32_t) { return false; }
bool Rtc::cancelWakeTimer() { return false; }
bool Rtc::readWakeTimer(bool& fired) {
  fired = false;
  return false;
}
bool Rtc::adjust(int32_t, DateTime*) { return false; }
}  // namespace freeink

#endif  // FREEINK_CAP_RTC
