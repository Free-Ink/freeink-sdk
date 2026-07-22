#pragma once

// FreeInk SDK — Xteink X3/X4 runtime detection.
//
// The Xteink X3 and X4 are two BoardProfiles compiled into one ESP32-C3 binary.
// They share a pinout but differ in panel controller (X3 = UC8253 792x528,
// X4 = SSD1677 800x480) and battery backend, so the running firmware must pick
// the right one before bringing up the display and SD card. The SDK leaves
// detection to the consumer (BoardConfig.h header note); this helper supplies
// the canonical Xteink fingerprint so dual X3/X4 apps don't each reinvent it.
//
// Detection probes the X3-only I2C peripherals on SDA=20 / SCL=0 — the BQ27220
// fuel gauge (0x55), DS3231 RTC (0x68) and QMI8658 IMU (0x6B/0x6A). The X4 has
// none of them, so two passes scoring >= 2 hits each confirm an X3; anything
// else is treated as an X4 (the conservative default).
//
// In builds without an Xteink profile (neither FREEINK_DEVICE_X4 nor
// FREEINK_DEVICE_X3) both functions compile to no-ops returning false and never
// touch a pin: the probe bus (SDA=20 / SCL=0) is only safe on the Xteink C3
// pinout — on an ESP32-S3 those are native USB D+ and the boot strap.

#include <stdint.h>

namespace freeink {

// Run the X3 I2C fingerprint and return true if this board is an Xteink X3.
// Leaves the I2C bus released and the probe pins back in INPUT mode. Safe to
// call before any other hardware bring-up.
bool detectXteinkIsX3();

// Convenience: run detectXteinkIsX3(), set BoardConfig::ACTIVE to the matching
// profile via selectDevice(), and return whether an X3 was detected (so the
// caller can put FreeInkDisplay in X3 mode with setDisplayX3()). Call this
// before SDCardManager::begin() and FreeInkDisplay::begin() so both read the
// correct profile.
bool selectXteinkDevice();

}  // namespace freeink
