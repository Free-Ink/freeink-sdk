#pragma once

#include <cstdint>

namespace freeink {

// Overlay uses the existing base + LSB/MSB mask API. Absolute requires planes
// containing every pixel, including the B/W background. Direct uses that same
// encoding with one combined activation using the full-quality image waveform.
// Querying selects no mode.
enum class GrayscaleMode : uint8_t { Overlay, Absolute, Direct };
enum class GrayscaleEncoding : uint8_t { Unsupported, OverlayMasks, AbsolutePlanes };
enum class GrayscaleBase : uint8_t { Separate, Combined };

struct GrayscaleCapabilities {
  // Overlay masks: black/white=00 (distinguished by the B/W base), dark=11,
  // light=01, in (LSB, MSB) order. Absolute planes: black=00, dark=10,
  // light=01, white=11. Encoding does not promise independence from prior ink.
  GrayscaleEncoding encoding = GrayscaleEncoding::Unsupported;
  GrayscaleBase base = GrayscaleBase::Separate;
  bool stripUploads = false;
  // An ordinary asynchronous B/W refresh is a valid base for this mode.
  bool asyncBase = false;
  // Plane staging touches no SPI and is safe during a pending B/W waveform.
  bool stagingWhileBusy = false;

  constexpr bool supported() const { return encoding != GrayscaleEncoding::Unsupported; }
};

namespace gray16 {

// Plane bitmasks indexed by 4-bit glyph ink 0..15 (0 = white, 15 = black).
// kMask4BitBw, kMaskBwClear, and kMaskCleanupBlack mark ink bits cleared to 0;
// kMask4BitGray* and kMaskPass*Dtm* mark bleach bits set to 1.
inline constexpr uint16_t kMask4BitBw = 0xFFFCu;
inline constexpr uint16_t kMask4BitGrayLsb = 0x0F80u;
inline constexpr uint16_t kMask4BitGrayMsb = 0x0FFCu;
inline constexpr uint16_t kMaskBwClear = 0xFF00u;
inline constexpr uint16_t kMaskPass1Dtm1 = 0x24EEu;
inline constexpr uint16_t kMaskPass1Dtm2 = 0x3756u;
inline constexpr uint16_t kMaskPass2Dtm1 = 0x71DAu;
inline constexpr uint16_t kMaskPass2Dtm2 = 0x6D0Au;
inline constexpr uint16_t kMaskCleanupBlack = 0xFFFEu;

// Maps 2-bit glyph ink (0..3) onto 4-bit ink anchors {0, ink1, ink2, 15}.
constexpr uint16_t permute2BitToGray16Mask(uint16_t baseMask4Bit, uint8_t ink1, uint8_t ink2) {
  return static_cast<uint16_t>((((baseMask4Bit >> 0) & 1u) << 0) | (((baseMask4Bit >> (ink1 & 0x0Fu)) & 1u) << 1) |
                               (((baseMask4Bit >> (ink2 & 0x0Fu)) & 1u) << 2) | (((baseMask4Bit >> 15) & 1u) << 3));
}

// Maps 2-bit glyph ink (0..3) onto the default 4-bit ink anchors (0, 6, 9, 15).
constexpr uint16_t mask2BitFrom4Bit(uint16_t mask4) { return permute2BitToGray16Mask(mask4, 6, 9); }

// Maps a 2-bit image value (0 = black, 1 = dark gray, 2 = light gray, 3 = white) to 4-bit ink (15, 9, 6, 0).
constexpr uint8_t image2BitToInk4(uint8_t val) {
  constexpr uint8_t kImage2BitInks[4] = {15, 9, 6, 0};
  return kImage2BitInks[val & 3u];
}

constexpr bool maskHasImage2Bit(uint16_t mask4, uint8_t val) {
  return (mask4 & static_cast<uint16_t>(1u << image2BitToInk4(val))) != 0;
}

// Maps 5-bit plane tuple (bw, coarseLsb, coarseMsb, fineLsb, fineMsb) to 4-bit gray (0 = black .. 15 = white).
inline constexpr uint8_t kDecodeGray16[32] = {
    0, 4, 0, 1, 6, 0, 3, 7, 0, 0, 0, 0, 0, 5, 0, 2, 15, 15, 15, 15, 15, 15, 11, 15, 10, 15, 8, 12, 13, 15, 9, 14,
};

}  // namespace gray16

}  // namespace freeink
