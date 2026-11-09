#pragma once

#include <cstdint>

#define FREEINK_SD_SDMMC 0
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_MCU_S3 0
#define FREEINK_MCU_ESP32 0
#define USE_BLOCK_DEVICE_INTERFACE 0
#define HAS_SDIO_CLASS 0

namespace BoardConfig {
struct Profile {
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
