#include <BoardConfig.h>

#if FREEINK_DEVICE_EMINIMAL

#include <It8951Config.h>

namespace freeink {

// e-Minimal 7.8": the IT8951 wiring and per-panel values the EMINIMAL_78 profile
// cannot carry. Every number here was measured on the bench at G2 (2026-09-18)
// against the Waveshare 7.8inch e-Paper HAT that ships with this device.
//
// The waveform mode NUMBERS are per LUT, not per controller. This panel reports
// LUT "M841", which orders them INIT 0 / DU 1 / GC16 2 / GL16 3 / GLR16 4 /
// GLD16 5 / A2 6 / DU4 7 -- M5Paper's LUT swaps the last two (DU4 6, A2 7), which
// is why the driver's default grayMode of 6 would run our anti-aliased pages
// through the 2-level A2 waveform and flatten every gray edge.
const It8951Config& eminimal78It8951Config() {
  static const It8951Config cfg = {
      17,        // miso: the HAT's own MISO. The driver default (13) is this board's
                 // SD D0 -- the card is on SDMMC, the panel has a bus of its own.
      13333333,  // spiHz: 13.3 MHz (the S3 rounds a request DOWN to 80/N; 13.33 is
                 // 80/6, so ask for exactly that). Clean on every G2 run at 10 and
                 // 13.3; 16 stalled HRDY on one run in four; 20 corrupts the 20-word
                 // GET_DEV_INFO read. Push 1107 -> 842 ms per 4bpp frame. Re-sweep
                 // on a PCB.
      IT8951_ROTATE_AUTO,  // panel reports 1872x1404 landscape -> rotation 0
      1920,      // vcomMv: -1.92 V from the FPC's QR sticker. The controller shipped
                 // at -2.50 V, which left a checkerboard ghost through a GC16; and
                 // an SPI-set VCOM does not survive a reset, so begin() must set it
                 // on every boot -- which it does.
      2,         // fullMode  = GC16, 525 ms. The ghost-clear.
      1,         // fastMode  = DU, 299 ms, 2-level differential. A2 (6) is 192 ms
                 // but ghosts harder; judge on real pages at G4 before switching.
      7,         // grayMode  = DU4 on M841 (see above), 359 ms, 4-level direct.
      8,         // ghostClearInterval: GC16 every 8 differential refreshes.
      0x00124850,  // imgBufFallbackAddr: this HAT's buffer base per GET_DEV_INFO.
      It8951WakeScrub::WhiteGc16,  // wake from the sleep cover with INIT + white GC16.
                 // Judged on real pages 2026-09-18: the default scrub (INIT + white
                 // GC16 + INIT, 4.8 s of a 7.5 s wake) left no shadow; the boot INIT
                 // alone ghosted, and so did INIT + INIT -- even though G2 saw one
                 // INIT clear a checkerboard on this glass, the cover's black field
                 // needs the GC16 drive to white. Under test: whether the trailing
                 // INIT adds anything; flip to WhiteGc16AndInit if this ghosts.
      It8951LoadDepth::Bpp1,  // B/W frames as the IT8951's 1bpp bitmap: 328 KB per
                 // frame instead of 1.31 MB, ~145 ms at 20 MHz instead of 580;
                 // exact black and white from the BGVR colour table. Gray (AA)
                 // pages stay 4bpp. Judged on glass 2026-09-20, first flash: Home,
                 // menu moves, book pages and back -- right way up, text intact,
                 // clean page turns, no ghosting. The push has not been timed yet.
                 // If it misbehaves on other glass or a PCB, the signatures are:
                 //   inverted (black page, white text)  -> BGVR_BW 0x00F0 -> 0xF000
                 //   text scrambled inside 8-px groups  -> BITMAP_LSB_FIRST -> false
                 //   menu band wrong, whole frame right  -> BITMAP_ROW_ALIGN / the
                 //                                          partial-load rule
                 //   anything else                       -> Bpp4, and note it here
                 // (the three constants are at the top of It8951Driver.cpp).
                 // 2bpp was tried 2026-09-18: the controller expands a 2-bit v to
                 // v<<2, not v*5 (datasheet fig. 7-16 -- the chip, not the firmware):
                 // white landed at 0xC, every page came up grey. Not an option.
      20000000,  // loadSpiHz: the bulk image write at 20 MHz (80/4). G2 measured the
                 // 4bpp push clean at 20 (580 ms) while the 20-word GET_DEV_INFO
                 // read failed there -- reads run out first on jumpers -- so only
                 // the write-only burst takes the fast clock; commands, reads and
                 // HRDY-paced words stay on spiHz. Under test: a multi-second
                 // freeze (HRDY stall) means back to 0 = spiHz. Re-sweep on a PCB.
  };
  return cfg;
}

}  // namespace freeink

#endif  // FREEINK_DEVICE_EMINIMAL
