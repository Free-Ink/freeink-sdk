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

  // Set brightness as a 0-100 percentage, mapped to duty through a perceptual
  // gamma-1.6554 curve (1% is the smallest non-zero duty step, not 1% linear
  // duty). 0 turns the light off. On a warm/cool board this is the TOTAL
  // brightness; the current color-temperature split is preserved.
  void setBrightness(uint8_t percent);

  // Set brightness with 8-bit control and a perceptual curve. Level 1 maps to the
  // smallest non-zero hardware duty, making dim night reading possible.
  void setBrightnessLevel(uint8_t level);

  // Convenience: fully off / restore last brightness.
  void off();
  void on();

  // Park the light for deep sleep: stop the PWM, then drive and LATCH the output
  // pin(s) at the LED's inactive level (gpio_hold_en, so the level survives the
  // sleep). Call it on the way into deep sleep, before the rails go down.
  //
  // Not the same as off(): off() writes a zero duty, but the pad is still owned
  // by LEDC and sleep entry hands it to esp_sleep_config_gpio_isolate(), which
  // floats it — what the light does then is down to an external pull, a
  // board-layout detail. A frontlight left lit is the single largest load a
  // sleeping reader can carry, so it is driven off rather than assumed off.
  // begin() releases the hold on the next boot.
  void prepareForDeepSleep();

  // Warm/cool mix, 0 = fully cool, 100 = fully warm, 50 = neutral. Only meaningful on a
  // two-channel board (hasColorTemperature()); a no-op on single-channel frontlights.
  void setColorTemperature(uint8_t warmPercent);

  bool present() const {
#if FREEINK_CAP_FRONTLIGHT
    // A PMIC-driven frontlight (Paper Mono: PM1 PWM0 -> AW9967) has no ESP
    // GPIO, so viaPm1Pwm counts as present alongside the LEDC-pin boards.
    return BoardConfig::ACTIVE.frontlight.gpio != BoardConfig::PIN_UNASSIGNED ||
           BoardConfig::ACTIVE.frontlight.viaPm1Pwm;
#else
    return false;  // frontlight code not compiled in (FREEINK_CAP_FRONTLIGHT=0)
#endif
  }

  // True when the board wires a second (warm) channel, so setColorTemperature() does
  // something. False on single-channel frontlights and on boards with none.
  bool hasColorTemperature() const {
#if FREEINK_CAP_WARMLIGHT
    return BoardConfig::ACTIVE.frontlight.gpio != BoardConfig::PIN_UNASSIGNED &&
           BoardConfig::ACTIVE.frontlight.gpioWarm != BoardConfig::PIN_UNASSIGNED;
#else
    return false;  // no warm-channel board in this build (FREEINK_CAP_WARMLIGHT=0)
#endif
  }

  uint8_t brightness() const { return _brightness; }
  uint8_t brightnessLevel() const { return _brightnessLevel; }
  uint8_t colorTemperature() const { return _warmPercent; }

 private:
#if FREEINK_CAP_FRONTLIGHT
  // Recompute and write both channels from _brightness + _warmPercent.
  void apply();
#endif
#ifdef FREEINK_FRONTLIGHT_LS
  // Keep RC_FAST powered through light sleep only while the light is actually
  // lit. The LEDC driver's KEEP_ALIVE config pins RC_FAST (and the digital
  // domain at its higher sleep bias) for every light-sleep window from boot;
  // begin() cancels that via the refcounted sleep sub-mode API and apply()
  // re-arms it on 0<->nonzero total-duty transitions, so dark idle sleeps at
  // full depth.
  void updateLsKeepAlive(bool lit);
  bool _lsAttachOk = false;         // both channel attaches succeeded (refcount is balanced)
  bool _lsKeepAliveArmed = false;   // our own +1 on the RC_FAST sleep sub-mode is active
#endif

  bool _begun = false;
  uint8_t _brightness = 0;
  uint8_t _brightnessLevel = 0;
  bool _useLevel = false;
  // Duty written by the previous apply(); 0 = the light was off. The off->on
  // edge is what arms the boost kick-start (see apply()).
  uint32_t _lastTotalDuty = 0;
  uint8_t _lastBrightness = 50;
  uint8_t _warmPercent = 50;  // neutral by default
};
