#pragma once
#include <cstdint>
namespace BoardConfig {
enum class RtcType { None, Pcf8563, Pcf85063, Ds3231, Rx8130, Rx8010 };
struct Sensors {
  int i2cSda = 47, i2cScl = 48;
  uint32_t i2cHz = 100000;
  uint8_t rtcAddr = 0x32, i2cBus = 0;
  RtcType rtcType = RtcType::Rx8130;
};
struct Profile {
  Sensors sensors;
};
inline Profile ACTIVE;
}  // namespace BoardConfig
