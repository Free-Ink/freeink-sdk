#pragma once

// FreeInk inertial measurement unit (LSM6DS3TR-C, QMI8658 or BMI270, 6-axis
// accel + gyro), or SC7A20H (acceleration only).
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
    float gx, gy, gz;  // angular rate, degrees/second; zero on accelerometer-only parts
  };

  // Verifies WHO_AM_I and configures accel + gyro for the active board.
  // Returns false when the active board has no IMU or the part doesn't identify.
  // BMI270: after a power cycle the part needs its 8 KiB configuration file
  // (~0.8 s on a 100 kHz bus) before it samples. begin() only identifies it and
  // leaves it in its power-on suspend state; the upload runs on the first
  // wake() or read(), so boards that never use the IMU don't pay for it.
  bool begin();
  bool present() const { return begun_; }
  bool hasGyroscope() const;  // false before begin() or on SC7A20H

  // Reads one sample. SC7A20H returns false when asleep or no new sample is
  // ready; gyro fields are zero. All backends return false on I2C error.
  bool read(Sample& out);

  // Puts the sensors into hardware standby / power-down. Config registers are
  // retained, so wake() restores sampling without a full begin(). Returns
  // false when the IMU is absent or on I2C error.
  bool sleep();

  // Restarts sampling after sleep(). Returns false when absent or on I2C
  // error; allow for a settling transient before trusting samples.
  bool wake();

 private:
  bool begun_ = false;
  bool sleeping_ = false;
  bool configured_ = false;  // BMI270: configuration file loaded, sampling set up
  // QMI8658 uses 0x6A/0x6B; SC7A20H uses 0x18/0x19 depending on its SA0
  // strap. Keep the address found by begin() instead of repeatedly using the
  // board profile's preferred address.
  uint8_t addr_ = 0;
};

}  // namespace freeink

using Imu = freeink::Imu;
