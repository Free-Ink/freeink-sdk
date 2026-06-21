#pragma once

// FreeInk temperature + humidity sensor (Sensirion SHT40).
//
// Reads ambient temperature (°C) and relative humidity (%) from the I2C sensor
// described by BoardConfig::ACTIVE.sensors (tempHumidityAddr / sensor bus).
// Dependency-free Wire access, mirroring BatteryMonitor. Boards without the
// sensor (FREEINK_CAP_TEMP_HUMIDITY off, or tempHumidityAddr == 0) link stub
// bodies and present() returns false.

#include <Arduino.h>

#include <cstdint>

namespace freeink {

class EnvironmentSensor {
 public:
  bool begin();
  bool present() const { return begun_; }

  // High-precision single-shot read. Returns false on I2C error or CRC mismatch.
  // tempC is degrees Celsius; humidityPct is 0-100 %RH (clamped).
  bool read(float& tempC, float& humidityPct);

 private:
  bool begun_ = false;
};

}  // namespace freeink

using EnvironmentSensor = freeink::EnvironmentSensor;
