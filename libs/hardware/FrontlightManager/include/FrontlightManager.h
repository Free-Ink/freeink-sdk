#pragma once

// FreeInk SDK — frontlight manager.
//
// Drives a PWM frontlight described by BoardConfig::ACTIVE.frontlight. Inert on
// boards without one (e.g. Xteink X4/X3), so it is always safe to construct.
//
// Scope: the single brightness-PWM path is functional now (used by the de-link
// board's primary LED). Tunable warm/cool color mixing and the boost-driver
// fault/OVP sensing (de-link's GPIO6/7/17/18) are scaffolded as no-op hooks for
// a follow-up.

#include <Arduino.h>
#include <BoardConfig.h>

class FrontlightManager {
 public:
  // Bring up the PWM channel. No-op if the board has no frontlight.
  void begin();

  // Set brightness as a 0-100 percentage. 0 turns the light off.
  void setBrightness(uint8_t percent);

  // Convenience: fully off / restore last brightness.
  void off();
  void on();

  // Warm/cool mix, 0 = fully cool, 100 = fully warm. Scaffold (no-op until the
  // multi-channel boost-driver path lands).
  void setColorTemperature(uint8_t warmPercent);

  bool present() const {
#if FREEINK_CAP_FRONTLIGHT
    return BoardConfig::ACTIVE.frontlight.gpio != BoardConfig::PIN_UNASSIGNED;
#else
    return false;  // frontlight code not compiled in (FREEINK_CAP_FRONTLIGHT=0)
#endif
  }
  uint8_t brightness() const { return _brightness; }

 private:
  bool _begun = false;
  uint8_t _brightness = 0;
  uint8_t _lastBrightness = 50;
  uint8_t _warmPercent = 50;
};
