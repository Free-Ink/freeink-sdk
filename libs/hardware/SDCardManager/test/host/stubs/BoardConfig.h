#pragma once

#include <cstdint>

#ifndef FREEINK_SD_SDMMC
#define FREEINK_SD_SDMMC 0
#endif
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_MCU_S3 0
#define FREEINK_MCU_ESP32 0
#ifndef USE_BLOCK_DEVICE_INTERFACE
#define USE_BLOCK_DEVICE_INTERFACE 0
#endif
#define HAS_SDIO_CLASS 0

namespace BoardConfig {
struct SdmmcPins {
  int clk = 1, cmd = 2, d0 = 3, d1 = 4, d2 = 5, d3 = 6;
};
struct Profile {
  SdmmcPins sdmmc;
  struct {
    int cs = 1, sclk = 2, mosi = 3, miso = 4, powerEnable = -1;
    bool separateSpi = false, powerActiveHigh = true;
    uint32_t spiHz = 0;
  } sd;
  struct {
    int cs = -1, sclk = -1, mosi = -1;
  } display;
  const char* name = "Host test";
};
inline Profile ACTIVE;
inline bool isOnePage() { return false; }
}  // namespace BoardConfig
