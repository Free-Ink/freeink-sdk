#pragma once

#include <cstdint>

namespace freeink {

// One-pass text that rests: in one refresh each pixel takes one of four drives, chosen from what the
// glass holds and what the page wants. A pixel already at its target rests, so black that stays black is
// not driven again and static text and the status bar do not collect charge page after page. The others
// go to white, to black, or take the gray row. Each driver sends the four as its own two planes.
//
// Eight pixels per byte. A set bit is white in `page` and `glass`, and gray in `pageGray` and `glassGray`;
// a gray bit wins over the B/W bit under it. `glassGray` is 0 where no gray is known on the glass. With
// `grayRests` false, gray takes the gray row on every page, as the UC8253 Murphy's text bank was tuned.
struct RestTextDrives {
  uint8_t rest;
  uint8_t toWhite;
  uint8_t toGray;
  uint8_t toBlack;
};

inline RestTextDrives restTextDrives(const uint8_t page, const uint8_t pageGray, const uint8_t glass,
                                     const uint8_t glassGray, const bool grayRests) {
  const auto white = static_cast<uint8_t>(page & ~pageGray);
  const auto black = static_cast<uint8_t>(~page & ~pageGray);
  const auto wasWhite = static_cast<uint8_t>(glass & ~glassGray);
  const auto wasBlack = static_cast<uint8_t>(~glass & ~glassGray);
  const auto grayStays = static_cast<uint8_t>(grayRests ? pageGray & glassGray : 0);
  const auto rest = static_cast<uint8_t>((white & wasWhite) | (black & wasBlack) | grayStays);
  return {rest, static_cast<uint8_t>(white & ~rest), static_cast<uint8_t>(pageGray & ~grayStays),
          static_cast<uint8_t>(black & ~rest)};
}

}  // namespace freeink
