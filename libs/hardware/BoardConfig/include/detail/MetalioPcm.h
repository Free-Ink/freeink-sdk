#pragma once

#include <cstdint>

namespace freeink { namespace metalio {
// Match the vendor's squared software volume and 32-bit I2S slots without
// shifting a negative signed value (undefined behavior in C++).
inline int32_t outputSample(int16_t sample, uint8_t volume) {
  const uint32_t v = volume > 100 ? 100 : volume;
  const int32_t gain = v * v * 65536u / 10000u;
  return static_cast<int32_t>(static_cast<int64_t>(sample) * gain);
}

inline int16_t inputSample(int32_t sample) {
  // The vendor takes the left slot, shifted by 12, with saturation.
  const int32_t value = sample >> 12;
  return value > INT16_MAX ? INT16_MAX : value < INT16_MIN ? INT16_MIN : static_cast<int16_t>(value);
}
} }  // namespace freeink::metalio
