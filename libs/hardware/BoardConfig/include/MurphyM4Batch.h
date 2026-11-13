#pragma once

#include <cstdint>

namespace freeink {

enum class MurphyM4Batch : uint8_t { First, Second };

constexpr MurphyM4Batch defaultMurphyM4Batch() {
#if defined(FREEINK_MURPHY_M4_BATCH1) && FREEINK_MURPHY_M4_BATCH1
  return MurphyM4Batch::First;
#else
  return MurphyM4Batch::Second;
#endif
}

struct MurphyM4TouchRange {
  int16_t min;
  int16_t max;
};

constexpr MurphyM4TouchRange murphyM4TouchRange(const MurphyM4Batch batch) {
  switch (batch) {
    case MurphyM4Batch::First:
      return {-52, 553};
    case MurphyM4Batch::Second:
      return {-47, 514};
  }
  return {-47, 514};
}

constexpr uint16_t mapMurphyM4TouchShortAxis(const uint16_t raw, const MurphyM4Batch batch, const uint16_t outMax) {
  const MurphyM4TouchRange range = murphyM4TouchRange(batch);
  const int32_t mapped =
      (static_cast<int32_t>(raw) - range.min) * outMax / (static_cast<int32_t>(range.max) - range.min);
  if (mapped <= 0) return 0;
  if (mapped >= outMax) return outMax;
  return static_cast<uint16_t>(mapped);
}

}  // namespace freeink
