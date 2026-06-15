#pragma once

// FreeInk real-time clock (PCF8563).
//
// Reads/sets wall-clock time on the I2C RTC described by
// BoardConfig::ACTIVE.sensors (rtcAddr / sensor bus pins). Dependency-free
// Wire access, mirroring BatteryMonitor. Boards without an RTC (FREEINK_CAP_RTC
// off, or rtcAddr == 0) link stub bodies and present() returns false.

#include <Arduino.h>

#include <cstdint>

namespace freeink {

class Rtc {
 public:
  // Wall-clock value. `year` is the full year (e.g. 2026); `weekday` is 0=Sunday.
  struct DateTime {
    uint16_t year = 2000;
    uint8_t month = 1;  // 1-12
    uint8_t day = 1;    // 1-31
    uint8_t hour = 0;   // 0-23
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t weekday = 0;  // 0=Sunday .. 6=Saturday
  };

  // Brings up the I2C bus and disables the RTC's CLKOUT. Returns false when the
  // active board has no RTC or the device doesn't ACK.
  bool begin();
  bool present() const { return begun_; }

  // Reads the current time. Returns false on I2C error or if the RTC reports its
  // oscillator stopped (low voltage / never set) — the time is then unreliable.
  bool now(DateTime& out);

  // Sets the time. Returns false on I2C error.
  bool set(const DateTime& dt);

 private:
  bool begun_ = false;
};

}  // namespace freeink

using Rtc = freeink::Rtc;
