#pragma once

// FreeInk real-time clocks selected by BoardConfig.
//
// Reads/sets wall-clock time on the I2C RTC described by
// BoardConfig::ACTIVE.sensors (rtcAddr / sensor bus pins). Dependency-free
// Wire access, mirroring BatteryMonitor. Boards without an RTC (FREEINK_CAP_RTC
// off, or rtcAddr == 0) link stub bodies and present() returns false.
//
// RtcType::Cw32L010Pmu (Read Pico) is NOT a register map: the CW32L010 answers a
// length-prefixed CRC frame protocol (PMU_CMD_TIME_GET / TIME_SYNC,
// read_pico_pmu_protocol.h), so the board installs the PMU time hooks below
// instead of this library reaching into the PMU itself (dependency direction:
// board -> libs).

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

  // Brings up the I2C bus and probes the selected RTC. Returns false when the
  // active board has no RTC or the device doesn't ACK.
  bool begin();
  bool present() const { return begun_; }

  // Reads the current time. Returns false on I2C error or if the RTC reports its
  // oscillator stopped (low voltage / never set) — the time is then unreliable.
  bool now(DateTime& out);

  // Sets the time. Returns false on I2C error.
  bool set(const DateTime& dt);

  // RX8130 countdown only. Above 65535 seconds, round up to whole minutes.
  // Requires a running, initialized clock and no other enabled RTC interrupt.
  bool armWakeTimer(uint32_t seconds);
  // Stops the countdown and clears only its event; verifies TE and TIE are off.
  bool cancelWakeTimer();
  // Reads the enabled timer event without clearing it; fired is false on error.
  bool readWakeTimer(bool& fired);

  // Shifts the running clock by a signed number of seconds, calendar-correct
  // across midnight/month/year boundaries (e.g. a time-zone change from
  // settings: adjust(deltaMinutes * 60)). Reads, shifts, writes back; `out`,
  // when given, receives the new time so callers can refresh UI state in one
  // call. Returns false when the RTC is absent or the read/write fails.
  bool adjust(int32_t seconds, DateTime* out = nullptr);

#if FREEINK_DEVICE_READPICO
  // --- PMU time source (BoardConfig::RtcType::Cw32L010Pmu) --------------------
  // Boards whose RTC is reached through a device protocol rather than an I2C
  // register map register these hooks. The board is the side that already talks
  // to the PMU (BoardReadPico::pmuTimeGet / pmuTimeSet on Read Pico), so this
  // library stays device-agnostic — the same split as
  // BatteryMonitor::setPmuBatteryHook and SDCardManager::setPowerHook.
  //
  //   * getUnix: read the device's unix seconds into `unixSec` and its own
  //     calibrated flag into `synced`. Returns false when the device does not
  //     answer the request at all.
  //   * setUnix: write `unixSec` (UTC) into the device. Returns false on any
  //     transfer or device error; the caller keeps the system clock either way.
  struct PmuTimeHooks {
    bool (*getUnix)(uint32_t& unixSec, bool& synced);
    bool (*setUnix)(uint32_t unixSec);
  };
  static void setPmuTimeHooks(const PmuTimeHooks& hooks);
#endif

 private:
  bool begun_ = false;
};

}  // namespace freeink

using Rtc = freeink::Rtc;
