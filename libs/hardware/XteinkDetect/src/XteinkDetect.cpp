#include "XteinkDetect.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Wire.h>

namespace freeink {

#if !(FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3)

// Neither Xteink profile is in this build, so there is nothing to fingerprint.
// Probing would also be unsafe here: SDA=20 / SCL=0 are only free pins on the
// Xteink C3 pinout — on an ESP32-S3, GPIO20 is native USB D+ and GPIO0 is the
// boot strap.
bool detectXteinkIsX3() { return false; }
bool selectXteinkDevice() { return false; }

#else

namespace {

// X3-only peripherals on the secondary I2C bus (SDA=20, SCL=0).
constexpr int X3_I2C_SDA = 20;
constexpr int X3_I2C_SCL = 0;
constexpr uint32_t X3_I2C_FREQ = 400000;

constexpr uint8_t ADDR_BQ27220 = 0x55;  // fuel gauge
constexpr uint8_t ADDR_DS3231 = 0x68;   // RTC
constexpr uint8_t ADDR_QMI8658 = 0x6B;  // IMU
constexpr uint8_t ADDR_QMI8658_ALT = 0x6A;

constexpr uint8_t BQ27220_SOC_REG = 0x2C;
constexpr uint8_t BQ27220_VOLT_REG = 0x08;
constexpr uint8_t DS3231_SEC_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;

bool readReg8(uint8_t addr, uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  *out = Wire.read();
  return true;
}

bool readReg16LE(uint8_t addr, uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *out = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

// Each probe checks not just for an ACK but for a plausible value, so a stray
// pull-up or floating bus can't masquerade as a present chip.
bool probeBq27220() {
  uint16_t soc = 0;
  uint16_t mv = 0;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_SOC_REG, &soc) || soc > 100) return false;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_VOLT_REG, &mv)) return false;
  return mv >= 2500 && mv <= 5000;
}

bool probeDs3231() {
  uint8_t sec = 0;
  if (!readReg8(ADDR_DS3231, DS3231_SEC_REG, &sec)) return false;
  const uint8_t tens = (sec >> 4) & 0x07;
  const uint8_t ones = sec & 0x0F;
  return tens <= 5 && ones <= 9;  // valid BCD seconds
}

bool probeQmi8658() {
  uint8_t who = 0;
  if (readReg8(ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  if (readReg8(ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  return false;
}

uint8_t runProbePass() {
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);
  const uint8_t score =
      static_cast<uint8_t>(probeBq27220()) + static_cast<uint8_t>(probeDs3231()) + static_cast<uint8_t>(probeQmi8658());
  Wire.end();
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
  return score;
}

}  // namespace

bool detectXteinkIsX3() {
  const uint8_t pass1 = runProbePass();
  delay(2);
  const uint8_t pass2 = runProbePass();
  // X3 confirmed only when both passes see at least two of the three chips; the
  // X4 sees zero, so a single stray ACK never flips the result.
  return pass1 >= 2 && pass2 >= 2;
}

bool selectXteinkDevice() {
  const bool isX3 = detectXteinkIsX3();
  BoardConfig::selectDevice(isX3 ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);
  return isX3;
}

#endif  // FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3

}  // namespace freeink
