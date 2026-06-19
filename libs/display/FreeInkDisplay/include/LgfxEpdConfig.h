#pragma once

#include <Arduino.h>

namespace freeink {

// Board-supplied power glue, called from the LovyanGFX bus lifecycle. The board
// implements these (for example PCA9535 expander + TPS65185 PMIC on LilyGo T5 S3)
// and injects them in its LgfxEpdConfig. Any hook may be null.
struct LgfxEpdPowerHooks {
  bool (*prepare)();
  bool (*powerOn)();
  void (*powerOff)();
};

// LovyanGFX parallel-EPD wiring. Geometry is not here; it comes from the active
// BoardProfile, like all drivers. This carries only bus/panel specifics.
struct LgfxEpdConfig {
  int8_t dataPins[8];
  int8_t pinSph;
  int8_t pinSpv;
  int8_t pinOe;
  int8_t pinLe;
  int8_t pinCl;
  int8_t pinCkv;
  int8_t pinPwr;
  uint32_t busHz;
  uint8_t linePadding;
  uint8_t rotation;
  LgfxEpdPowerHooks power;
};

}  // namespace freeink
