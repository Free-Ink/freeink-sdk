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
  const uint32_t* lutQuality = nullptr;
  size_t lutQualityStep = 0;
  const uint32_t* lutText = nullptr;
  size_t lutTextStep = 0;
  const uint32_t* lutFast = nullptr;
  size_t lutFastStep = 0;
  const uint32_t* lutFastest = nullptr;
  size_t lutFastestStep = 0;

  // The 8-bit canvas values the driver writes for the two anti-aliasing greys.
  // Panel_EPD quantises a canvas byte to a 4-bit level as (v + bayer - 8) >> 4,
  // so only v == (level << 4) | 8 lands on one level for every cell of the Bayer
  // matrix; anything else alternates between two levels and shows up as speckle
  // along glyph edges. grayLevelByte() builds a safe value from a level.
  //
  // Which level to ask for is a property of the board's waveform: the LUT drives
  // grey destinations by column, so a canvas level the LUT has no column for
  // simply stays at whatever the B/W base left it on.
  //
  // The defaults are the even thirds this driver has always written. They are not
  // Bayer-exact, which does not matter on a board that leaves
  // grayNudgeInFastBank false: that path dithers the canvas anyway, and these are
  // the densities it has been dithering since the driver was written.
  uint8_t grayDark = 0x55;
  uint8_t grayLight = 0xAA;

  // True when the board's epd_fast LUT carries grey columns as well as the two
  // B/W rails, so the grayscale push can go out through the differential bank:
  // no lut_eraser flash, and the same bank the B/W base used, which is what lets
  // Panel_EPD's per-pixel diff keep skipping everything that did not change.
  //
  // Left false for a board on LovyanGFX's stock LUTs, whose fast bank drives only
  // the rails. Those boards keep the plain epd_fast push they have always had --
  // the greys still come out dithered there, which is a waveform gap on that
  // board, not something this driver can paper over.
  bool grayNudgeInFastBank = false;
};

// Canvas byte that quantises to exactly `level` for every Bayer cell.
constexpr uint8_t grayLevelByte(uint8_t level) {
  return static_cast<uint8_t>((level << 4) | 8);
}

}  // namespace freeink
