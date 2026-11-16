#pragma once

#include <stdint.h>

namespace freeink {

// EEGO A4 units the stock firmware (V1.0.3) treats as having no frontlight even
// though their LM3630A controller answers on I2C. Stock keys this on its device
// UID, the low 24 bits of the factory MAC (printed as "uid=" in its boot log):
// within 0xBB0408..0xBB0418, the set bits of 0x10011 mark the unlit units.
constexpr bool eegoA4UidHasNoFrontlight(const uint32_t uid) {
  return uid - 0xBB0408u <= 16u && ((0x10011u >> (uid - 0xBB0408u)) & 1u) != 0;
}

static_assert(eegoA4UidHasNoFrontlight(0xBB0408) && eegoA4UidHasNoFrontlight(0xBB040C) &&
                  eegoA4UidHasNoFrontlight(0xBB0418),
              "stock's unlit units");
static_assert(!eegoA4UidHasNoFrontlight(0xBB0409) && !eegoA4UidHasNoFrontlight(0xBB0417) &&
                  !eegoA4UidHasNoFrontlight(0xBB0407) && !eegoA4UidHasNoFrontlight(0xBB0419),
              "lit units and UIDs outside the table keep the I2C probe result");

}  // namespace freeink
