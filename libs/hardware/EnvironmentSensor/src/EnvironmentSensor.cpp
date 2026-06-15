#include "EnvironmentSensor.h"

#include <BoardConfig.h>

#if FREEINK_CAP_TEMP_HUMIDITY

#include <Wire.h>

namespace freeink {
namespace {

// SHT40 commands (datasheet). 0xFD = measure T+RH, high precision.
constexpr uint8_t CMD_MEASURE_HIGH_PRECISION = 0xFD;
constexpr uint8_t CMD_SOFT_RESET = 0x94;
constexpr uint32_t MEASURE_DELAY_MS = 10;  // high-precision conversion time (~8.3 ms max)

bool g_wireReady = false;
void ensureWire() {
  if (g_wireReady) return;
  const auto& s = BoardConfig::ACTIVE.sensors;
  Wire.begin(s.i2cSda, s.i2cScl, s.i2cHz);
  g_wireReady = true;
}

bool sendCommand(uint8_t addr, uint8_t cmd) {
  ensureWire();
  Wire.beginTransmission(addr);
  Wire.write(cmd);
  return Wire.endTransmission() == 0;
}

// Sensirion CRC-8: polynomial 0x31, init 0xFF, over the two data bytes.
uint8_t crc8(uint8_t msb, uint8_t lsb) {
  uint8_t crc = 0xFF;
  const uint8_t data[2] = {msb, lsb};
  for (uint8_t b = 0; b < 2; ++b) {
    crc ^= data[b];
    for (uint8_t i = 0; i < 8; ++i) crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
  }
  return crc;
}

}  // namespace

bool EnvironmentSensor::begin() {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.tempHumidityAddr;
  if (addr == 0) return false;
  ensureWire();
  if (!sendCommand(addr, CMD_SOFT_RESET)) return false;  // also probes the ACK
  delay(2);                                              // soft-reset settle (~1 ms)
  begun_ = true;
  return true;
}

bool EnvironmentSensor::read(float& tempC, float& humidityPct) {
  const uint8_t addr = BoardConfig::ACTIVE.sensors.tempHumidityAddr;
  if (!begun_ || addr == 0) return false;
  if (!sendCommand(addr, CMD_MEASURE_HIGH_PRECISION)) return false;
  delay(MEASURE_DELAY_MS);

  if (Wire.requestFrom(addr, static_cast<uint8_t>(6), static_cast<uint8_t>(true)) < 6) return false;
  uint8_t b[6];
  for (uint8_t i = 0; i < 6; ++i) b[i] = Wire.read();
  if (crc8(b[0], b[1]) != b[2] || crc8(b[3], b[4]) != b[5]) return false;

  const uint16_t tRaw = static_cast<uint16_t>(b[0] << 8 | b[1]);
  const uint16_t rhRaw = static_cast<uint16_t>(b[3] << 8 | b[4]);
  tempC = -45.0f + 175.0f * (static_cast<float>(tRaw) / 65535.0f);
  float rh = -6.0f + 125.0f * (static_cast<float>(rhRaw) / 65535.0f);
  if (rh < 0.0f) rh = 0.0f;
  if (rh > 100.0f) rh = 100.0f;
  humidityPct = rh;
  return true;
}

}  // namespace freeink

#else  // FREEINK_CAP_TEMP_HUMIDITY — sensor absent.

namespace freeink {
bool EnvironmentSensor::begin() { return false; }
bool EnvironmentSensor::read(float&, float&) { return false; }
}  // namespace freeink

#endif  // FREEINK_CAP_TEMP_HUMIDITY
