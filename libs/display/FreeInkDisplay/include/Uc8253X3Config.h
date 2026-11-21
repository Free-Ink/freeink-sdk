#pragma once

#include <cstdint>

namespace freeink {

// One UC8253 waveform bank: VCOM + the four transition LUTs.
struct Uc8253LutBank {
  const uint8_t* vcom = nullptr;
  const uint8_t* ww = nullptr;
  const uint8_t* bw = nullptr;
  const uint8_t* wb = nullptr;
  const uint8_t* bb = nullptr;
};

struct Uc8253Lut16Config {
  Uc8253LutBank preBwMid;      // 36-frame single-pass cross-dissolve base bank (CDI 0xA9, 0x07)
  Uc8253LutBank pass1Coarse;   // 5-frame coarse bank (CDI 0xA9, 0x07)
  Uc8253LutBank pass2Fine;     // 4-frame fine bank (CDI 0xA9, 0x07)
  uint8_t pllBase = 0x09;      // 80 Hz nominal for preBwMid cross-dissolve (restores 0x09 idle)
  uint8_t pllPass1 = 0x0B;     // 100 Hz nominal
  uint8_t pllPass2 = 0x0E;     // ~165 Hz nominal
  uint8_t vdlPass2 = 0x1E;     // CMD_POWER_SETTING byte[3] during Pass 2 (-8.40V)
  Uc8253LutBank step0Erase;    // Optional 28-frame Step 0 erase bank (disabled unless step0Enable is true; CDI 0x29)
  bool step0Enable = false;    // Default false = single-pass cross-dissolve; true runs Step 0 first
  uint8_t vcomPass1 = 0x24;    // CMD_VCOM_DC (0x82) during Pass 1 (-1.90V)
  uint8_t vcomPass2 = 0x22;    // CMD_VCOM_DC (0x82) during Pass 2 (-1.80V)
  Uc8253LutBank bipolarScrub;  // 62-frame single-DRF history-independent GC scrub bank (CDI 0xA9, 0x07)
};

struct Uc8253X3Config {
  Uc8253LutBank normal;    // condition-pass / settle (CDI 0xA9)
  Uc8253LutBank half;      // scrub (CDI 0xA9)
  Uc8253LutBank fast;      // turbo differential (CDI 0x29)
  Uc8253LutBank full;      // OEM full / factory (CDI 0x29)
  Uc8253LutBank gc;        // 5-frame 4-level grayscale nudge (CDI 0x29)
  Uc8253LutBank preBwMid;  // OEM grayscale preconditioning settle (CDI 0xA9)
  uint8_t lutLen = 42;     // bytes per LUT sent to the controller (42)
  const Uc8253LutBank* directGray = nullptr;
  Uc8253Lut16Config lut16 = {};
};

const Uc8253X3Config& uc8253X3DefaultConfig();

}  // namespace freeink
