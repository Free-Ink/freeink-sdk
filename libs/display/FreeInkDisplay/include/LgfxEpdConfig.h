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
};

// --- LUT block budget ------------------------------------------------------
//
// Panel_EPD packs a pixel's refresh progress into a uint16_t as
// (lut_block << 8) | level, and blit_dmabuf reads it back through a SIGNED cast
// and skips the pixel when the result is negative -- bit 15 means "this pixel is
// idle". So no block index may ever reach 128: the five banks together have to
// fit in 128 blocks, whichever bank a given refresh uses.
//
// Overrunning it fails SILENTLY and GLOBALLY. Every pixel whose waveform reaches
// block 128 reads as idle mid-refresh, so refreshes stop completing and the panel
// simply looks dead. Nothing reports it, and because a colder temperature range
// generally needs longer banks it can present as a board that works when warm and
// blanks when cool -- which is exactly how we met it, and it cost a debugging
// session. Hence the constants below and the check in LgfxEpdDriver::begin().
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_MAX = 128;

// Panel_EPD's hardcoded eraser bank, prepended to every epd_text / epd_quality
// refresh and not supplied by config (Panel_EPD.cpp, lut_eraser /
// lut_eraser_step): two drive rows, a park row and the terminator.
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_ERASER = 4;

// What Panel_EPD substitutes into a slot this config leaves empty. Counted from
// its stock tables; a board on all four pays 85 of the 128 blocks.
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_STOCK_QUALITY = 32;
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_STOCK_TEXT = 32;
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_STOCK_FAST = 10;
inline constexpr size_t LGFX_EPD_LUT_BLOCKS_STOCK_FASTEST = 7;

// Blocks this config will actually occupy, stock substitution included. Mirrors
// the accumulation in Panel_EPD::init_intenal().
constexpr size_t lgfxEpdLutBlocks(const LgfxEpdConfig& c) {
  return LGFX_EPD_LUT_BLOCKS_ERASER +
         ((c.lutQuality && c.lutQualityStep) ? c.lutQualityStep : LGFX_EPD_LUT_BLOCKS_STOCK_QUALITY) +
         ((c.lutText && c.lutTextStep) ? c.lutTextStep : LGFX_EPD_LUT_BLOCKS_STOCK_TEXT) +
         ((c.lutFast && c.lutFastStep) ? c.lutFastStep : LGFX_EPD_LUT_BLOCKS_STOCK_FAST) +
         ((c.lutFastest && c.lutFastestStep) ? c.lutFastestStep : LGFX_EPD_LUT_BLOCKS_STOCK_FASTEST);
}

}  // namespace freeink
