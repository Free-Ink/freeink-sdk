#pragma once

// FreeInk SDK — frontlight manager.
//
// Drives a PWM frontlight described by BoardConfig::ACTIVE.frontlight. Inert on
// boards without one (e.g. Xteink X4/X3), so it is always safe to construct.
//
// Two topologies, selected purely by the board profile:
//   * Single channel (de-link primary LED, LilyGo backlight, Murphy): one PWM pin
//     (frontlight.gpio). setColorTemperature() is a no-op.
//   * Warm/cool pair (Xteink X4 Pro: cool=GPIO8, warm=GPIO9): two independent PWM
//     channels. Overall brightness is the total light; color temperature splits that
//     total between the cool (gpio) and warm (gpioWarm) strings, so brightness stays
//     roughly constant as the color shifts. setColorTemperature() drives the mix.

#include <Arduino.h>
#include <BoardConfig.h>

class FrontlightManager {
 public:
  // Bring up the PWM channel(s). No-op if the board has no frontlight.
  void begin();

  // Set brightness as a 0-100 percentage. 0 turns the light off. On a warm/cool board
  // this is the TOTAL brightness; the current color-temperature split is preserved.
  void setBrightness(uint8_t percent);

  // Convenience: fully off / restore last brightness.
  void off();
  void on();

  // Warm/cool mix, 0 = fully cool, 100 = fully warm, 50 = neutral. Only meaningful on a
  // two-channel board (hasColorTemperature()); a no-op on single-channel frontlights.
  void setColorTemperature(uint8_t warmPercent);

  bool present() const {
#if FREEINK_CAP_FRONTLIGHT
    return BoardConfig::ACTIVE.frontlight.gpio != BoardConfig::PIN_UNASSIGNED;
#else
    return false;  // frontlight code not compiled in (FREEINK_CAP_FRONTLIGHT=0)
#endif
  }

  // True when the board wires a second (warm) channel, so setColorTemperature() does
  // something. False on single-channel frontlights and on boards with none.
  bool hasColorTemperature() const {
#if FREEINK_CAP_FRONTLIGHT
    return BoardConfig::ACTIVE.frontlight.gpio != BoardConfig::PIN_UNASSIGNED &&
           BoardConfig::ACTIVE.frontlight.gpioWarm != BoardConfig::PIN_UNASSIGNED;
#else
    return false;
#endif
  }

  uint8_t brightness() const { return _brightness; }
  uint8_t colorTemperature() const { return _warmPercent; }

 private:
#if FREEINK_CAP_FRONTLIGHT
  // Recompute and write both channels from _brightness + _warmPercent.
  void apply();
#endif

  bool _begun = false;
  uint8_t _brightness = 0;
  uint8_t _lastBrightness = 50;
  uint8_t _warmPercent = 50;  // neutral by default
};
