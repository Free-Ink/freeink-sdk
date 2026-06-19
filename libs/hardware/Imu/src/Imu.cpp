#include "Imu.h"

#include <BoardConfig.h>

#if FREEINK_CAP_IMU

#include <Wire.h>
#include <soc/soc_caps.h>

namespace freeink {
namespace {

// LSM6DS3TR-C register map (datasheet).
constexpr uint8_t REG_WHO_AM_I = 0x0F;
constexpr uint8_t WHO_AM_I_VALUE = 0x6A;  // shared by LSM6DS3 / LSM6DS3TR-C
constexpr uint8_t REG_CTRL1_XL = 0x10;    // accel: ODR + full scale
constexpr uint8_t REG_CTRL2_G = 0x11;     // gyro: ODR + full scale
constexpr uint8_t REG_CTRL3_C = 0x12;     // BDU / auto-increment
constexpr uint8_t REG_OUTX_L_G = 0x22;    // gyro X..Z (6 bytes, LE)
constexpr uint8_t REG_OUTX_L_XL = 0x28;   // accel X..Z (6 bytes, LE)

// 104 Hz (ODR = 0100b in bits [7:4]); accel FS = ±2 g, gyro FS = ±245 dps (00b).
constexpr uint8_t CTRL1_XL_104HZ_2G = 0x40;
constexpr uint8_t CTRL2_G_104HZ_245DPS = 0x40;
constexpr uint8_t CTRL3_C_BDU_IF_INC = 0x44;  // BDU=1, IF_INC=1 (block update + auto-increment)

// Sensitivities for the scales above (datasheet "mechanical characteristics").
constexpr float ACCEL_G_PER_LSB = 0.061f / 1000.0f;   // 0.061 mg/LSB at ±2 g
constexpr float GYRO_DPS_PER_LSB = 8.75f / 1000.0f;    // 8.75 mdps/LSB at ±245 dps

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

bool Imu::begin() {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.imuAddr;
  if (addr == 0) return false;
  const auto& s = BoardConfig::ACTIVE.sensors;
  if (s.i2cSda < 0 || s.i2cScl < 0 || s.i2cHz == 0) return false;
  ensureWire();
  uint8_t who = 0;
  if (!readRegs(addr, REG_WHO_AM_I, &who, 1) || who != WHO_AM_I_VALUE) return false;
  if (!writeReg(addr, REG_CTRL3_C, CTRL3_C_BDU_IF_INC)) return false;
  if (!writeReg(addr, REG_CTRL1_XL, CTRL1_XL_104HZ_2G)) return false;
  if (!writeReg(addr, REG_CTRL2_G, CTRL2_G_104HZ_245DPS)) return false;
  begun_ = true;
  return true;
}

bool Imu::read(Sample& out) {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.imuAddr;
  if (!begun_ || addr == 0) return false;
  uint8_t g[6];
  uint8_t a[6];
  if (!readRegs(addr, REG_OUTX_L_G, g, sizeof(g))) return false;
  if (!readRegs(addr, REG_OUTX_L_XL, a, sizeof(a))) return false;

  const int16_t gx = static_cast<int16_t>(g[0] | g[1] << 8);
  const int16_t gy = static_cast<int16_t>(g[2] | g[3] << 8);
  const int16_t gz = static_cast<int16_t>(g[4] | g[5] << 8);
  const int16_t ax = static_cast<int16_t>(a[0] | a[1] << 8);
  const int16_t ay = static_cast<int16_t>(a[2] | a[3] << 8);
  const int16_t az = static_cast<int16_t>(a[4] | a[5] << 8);

  out.ax = ax * ACCEL_G_PER_LSB;
  out.ay = ay * ACCEL_G_PER_LSB;
  out.az = az * ACCEL_G_PER_LSB;
  out.gx = gx * GYRO_DPS_PER_LSB;
  out.gy = gy * GYRO_DPS_PER_LSB;
  out.gz = gz * GYRO_DPS_PER_LSB;
  return true;
}

}  // namespace freeink

#else  // FREEINK_CAP_IMU — IMU absent.

namespace freeink {
bool Imu::begin() { return false; }
bool Imu::read(Sample&) { return false; }
}  // namespace freeink

#endif  // FREEINK_CAP_IMU
