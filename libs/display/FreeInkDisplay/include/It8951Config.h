#pragma once

#include <Arduino.h>

namespace freeink {

// cfg.rotation sentinel: pick 0° or 90° at begin() from the panel's reported
// orientation so the landscape framebuffer lands upright on the portrait panel.
constexpr uint16_t IT8951_ROTATE_AUTO = 0xFF;

// How begin() clears the glass when the host wakes from deep sleep with the
// sleep cover still showing. Every boot starts with one INIT; this is what
// follows it. Judge on real pages: the residue is a faint shadow of the cover
// in the white margins after a wake, and a torture-pattern result (eMinimal's
// M841 glass cleared a checkerboard in one INIT at G2) does not predict it —
// the cover ghosted after a single INIT on the same glass.
enum class It8951WakeScrub : uint8_t {
  WhiteGc16AndInit = 0,  // + white GC16 (a full-frame push) + INIT. Default; M5Paper.
  SecondInit,            // + INIT. Two passes without the push. Ghosted on eMinimal:
                         // the white GC16 is the pass that clears, not the INIT.
  None,                  // the boot INIT only. Cheapest; ghosted on eMinimal.
  WhiteGc16,             // + white GC16, no trailing INIT.
};

// Pixel depth of the image loads. The host's frames only ever carry four
// levels -- black, dark (0x5), light (0xA), white -- so 2bpp would be lossless
// if the controller expanded a 2-bit value v to the 4-bit v*5, and it halves
// the bytes on the wire: 1.31 MB -> 657 KB per full frame. Measured on the
// eMinimal 7.8" (IT8951E, LUT M841) 2026-09-18: it expands v<<2 -- white lands
// at 0xC, every page background comes up grey and a "white" GC16 ghosts. That
// is the chip, not the firmware: the datasheet (V0.2.4.3 fig. 7-16) stores the
// input bits in the top of an 8-bit pixel and zero-pads, for every depth below
// 8bpp. Only 4bpp and 8bpp reach 0xF. 2bpp is kept for a controller that maps
// it differently; do not enable without judging a page on glass.
//
// The lever that does work is the IT8951's 1bpp BITMAP mode: the frame memory
// holds one bit per pixel and the display engine paints the two levels from a
// colour-table register (BGVR), so black and white are exact and a full frame
// is a quarter of the 4bpp bytes (1.31 MB -> 328 KB, ~145 ms at 20 MHz). Only
// B/W frames can go this way; the anti-aliased gray frames stay 4bpp, and the
// driver switches the frame memory between the two formats as needed.
enum class It8951LoadDepth : uint8_t {
  Bpp4 = 0,  // native 16-gray packing, two pixels per byte. Default; M5Paper.
  Bpp2,      // four pixels per byte; levels 0/4/8/12 on the IT8951E tested.
  Bpp1,      // B/W frames as a 1bpp bitmap (eight pixels per byte, BGVR levels);
             // gray frames 4bpp. Needs load rotation 0 (the bitmap trick loads
             // bytes as if they were 8bpp pixels, which the rotator would scramble);
             // the driver falls back to 4bpp when the rotation is not 0.
};

// IT8951 wiring and per-panel values. Geometry is not here; it comes from the
// active BoardProfile (and GET_DEV_INFO at runtime), like all drivers. A board
// supplies one of these via -DFREEINK_IT8951_CONFIG=yourConfig, or the SDK's
// board-support library does (BoardEMinimal78); M5Paper uses the driver default.
//
// The waveform mode numbers are a property of the panel's LUT, not the
// controller: M5Paper's LUT has DU4 at 6 and A2 at 7, Waveshare's M841 has A2 at
// 6 and DU4 at 7. Read the LUT string GET_DEV_INFO returns before copying modes.
struct It8951Config {
  int8_t miso;        // SPI MISO (IT8951 register / device-info reads)
  uint32_t spiHz;     // SPI clock
  uint16_t rotation;  // LD_IMG rotation 0/1/2/3 = 0/90/180/270°, or IT8951_ROTATE_AUTO
  uint16_t vcomMv;    // VCOM magnitude in mV (e.g. 2300 = -2.30 V); 0 = keep panel OTP
  uint8_t fullMode;   // clearing refresh (GC16 = 2). Used for Full/Half, wake from
                      // standby, and the periodic ghost-clear below.
  uint8_t fastMode;   // B/W page turns (DU = 1, 2-level differential).
  uint8_t grayMode;   // anti-aliased grayscale pages. DU4 is a 4-level DIRECT
                      // update: differential, so only changed pixels (the AA glyph
                      // edges) move — no flash. GC16 (2) would drive every pixel.
  uint16_t ghostClearInterval;  // promote a differential (DU/DU4) refresh to a GC16
                                // ghost-clear every N whole frames of differential
                                // refresh (0 = never). A partial-area refresh (a
                                // menu highlight move) counts its share of the
                                // frame, at least a quarter, so a scroll does not
                                // flash every N presses the way N page turns do.
                                // Keeps DU/DU4 residue from accumulating across
                                // menu and activity navigation, with no firmware
                                // involvement.
  uint32_t imgBufFallbackAddr;  // used only if GET_DEV_INFO returns no buffer address
  It8951WakeScrub wakeScrub;    // what follows the boot INIT on a deep-sleep wake;
                                // see the enum. Value-initialised (0) = the default
                                // scrub for configs that do not name it.
  It8951LoadDepth loadDepth;    // bits per pixel on the wire for every image load.
                                // Value-initialised (0) = 4bpp, the native 16-gray.
  uint32_t loadSpiHz;           // SPI clock for the bulk image write only; 0 = spiHz.
                                // Reads (device info, registers, HRDY-paced words)
                                // are what fail first on long MISO runs, so a board
                                // can push frames faster than it can talk.
};

}  // namespace freeink
