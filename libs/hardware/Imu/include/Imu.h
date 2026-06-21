#pragma once

// FreeInk inertial measurement unit (ST LSM6DS3TR-C, 6-axis accel + gyro).
//
// Reads acceleration (g) and angular rate (deg/s) from the I2C IMU described by
// BoardConfig::ACTIVE.sensors (imuAddr / sensor bus). Dependency-free Wire
// access, mirroring BatteryMonitor. Boards without an IMU (FREEINK_CAP_IMU off,
// or imuAddr == 0) link stub bodies and present() returns false.

#include <Arduino.h>

#include <cstdint>

namespace freeink {

class Imu {
 public:
  struct Sample {
    float ax, ay, az;  // acceleration, g (1 g ~= 9.81 m/s^2)
    float gx, gy, gz;  // angular rate, degrees/second
  };

  // Verifies WHO_AM_I and configures accel (±2 g) + gyro (±245 dps) at 104 Hz.
  // Returns false when the active board has no IMU or the part doesn't identify.
  bool begin();
  bool present() const { return begun_; }

  // Reads one accel + gyro sample. Returns false on I2C error.
  bool read(Sample& out);

 private:
  bool begun_ = false;
};

}  // namespace freeink

using Imu = freeink::Imu;
