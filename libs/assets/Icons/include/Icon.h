#pragma once

#include <cstdint>

namespace freeink {

// A 1-bpp icon bitmap with baked-in layout metadata, generated from an SVG by
// tools/gen_icons.py (see libs/assets/Icons).
//
// Format: rows top-to-bottom, each (w + 7) / 8 bytes, MSB-first. Bit 1 = leave the
// pixel (transparent), bit 0 = draw black. NOT pre-rotated — the renderer maps
// logical coordinates to the panel itself, so the same asset is correct in every
// orientation.
//
// opticalCenterY is the row of the content's center of mass: align an icon's
// vertical center to a line of text by placing it so opticalCenterY lands on the
// text's optical center (e.g. GfxRenderer::getTextVisualCenterOffset()). Because it
// is measured from the real artwork, asymmetric icons (a clock, arrows) center
// correctly with no per-icon tweaking.
struct Icon {
  uint16_t w;
  uint16_t h;
  int16_t opticalCenterY;
  const uint8_t* bits;
};

}  // namespace freeink
