#pragma once

// UC8179 panel driver — Xteink X4 / X4 Pro, newer production run (800x480 B/W).
// UltraChip UC8179 driven in KW mode (PSR KW/R=1): 1-bpp, DTM1 = OLD plane,
// DTM2 = NEW plane, differential refresh — the same KW paradigm and command set
// as the UC8279d X3 driver. It is a *separate* driver because the UC8179 needs
// an explicit PLL / booster / VCOM bring-up that the UC8279d (pure-OTP) omits:
// on OTP defaults alone the UC8179 never develops an image.
//
// Recovered from the X4 Pro OEM firmware (UC8179_800x480 init FUN_4214dff8 /
// full-update FUN_4214e584, via Ghidra). It runs the factory OTP waveforms
// (PSR REG=0) — the MTP holds temperature-compensated LUT sets — so no custom
// LUT upload is needed; only the power rails are programmed here. PENDING
// HARDWARE VALIDATION on a UC8179 (screenType=1 / hw_calib=2) X4 / X4 Pro unit.
//
// BUSY_N: low while busy (PON/DRF/POF all flag), same two-phase shape as the
// UC8279d / UC8253 X3 — reuses BusyPolarity::X3TwoPhase and the async split.

#include "PanelDriver.h"

namespace freeink {

struct Uc8179Config {
  // PSR (0x00) byte 0, as written at INIT. The OEM writes 0x3B here (REG bit5=1)
  // and re-asserts (psr0 & 0xDF)=0x1B (REG=0 -> OTP LUTs) just before every PON.
  uint8_t psr0;
  // PSR (0x00) byte 1.
  uint8_t psr1;
  // PFS power-off sequence (cmd 0x03). (PLL is 0x30 and stays panel-programmed.)
  uint8_t pfs;
  // Booster soft-start (cmd 0x06), 4 bytes.
  uint8_t btst[4];
  // Gate-scan selection (cmd 0xE1).
  uint8_t gateScan;
  // CCSET cascade/output enable (cmd 0xE0).
  uint8_t ccset;
  // TSSET forced temperature (cmd 0xE5) for a full refresh — selects the OTP
  // waveform's frame count/rate.
  uint8_t tsset;
  // TSSET (cmd 0xE5) for a fast/partial refresh (the OEM uses a different value).
  uint8_t tssetFast;
  // CDI (0x50) byte0 asserted during a refresh (before DRF); byte1 is 0x07.
  uint8_t cdiActive;
  // CDI (0x50) byte0 restored after the refresh completes; byte1 is 0x07.
  uint8_t cdiIdle;
  // TRES (0x61) gate count. The X4 Pro panel is addressed as 800x600 even though
  // only 480 rows are visible — the OTP waveform is tuned for the full 600-gate
  // scan, so the DTM transfer is padded to this height. (Visible height comes
  // from the board profile.)
  uint16_t tresHeight;
};

const Uc8179Config& uc8179DefaultConfig();

class Uc8179Driver : public PanelDriver {
 public:
  explicit Uc8179Driver(const Uc8179Config& cfg = uc8179DefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::X3TwoPhase; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  bool supportsAsyncDisplay() const override { return true; }

  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;

  // --- 4-level grayscale (anti-aliasing) ---
  // Two full 1bpp planes encode 4 levels: LSB -> DTM 0x10 ("old"), MSB -> DTM
  // 0x13 ("new"); the OTP gray waveform resolves (old,new) -> {black, 2 mids,
  // white}. Full-buffer path only (supportsStripGrayscale stays false — the
  // UC8179 has no RAM-window addressing and our row-reversal orientation can't
  // span strips; the X4 Pro's PSRAM absorbs the two full planes).
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;

 private:
  void initController(EpdBus& bus);
  // Stream a framebuffer into a RAM plane (ramCmd): mirror-Y via row reversal
  // (sendPlaneFlipped, as the UC8279 sibling does), padded with white to the
  // addressed gate count. Mirror-X is done in hardware via the PSR SHL bit. Used
  // for both the NEW plane (0x13) and the OLD-plane sync (0x10).
  void streamPlane(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb);

  const Uc8179Config& _cfg;

  uint16_t _w;        // visible width (800)
  uint16_t _h;        // visible height (480)
  uint16_t _wb;       // width in bytes (100)
  uint16_t _tresH;    // addressed gate count (600) — DTM padded to this
  uint32_t _bufferSize;

  bool _isScreenOn = false;
  // Force the first refresh after begin() to a full flash, so a partial update
  // never runs against an unknown on-screen state (e.g. a retained boot image).
  bool _needFullClear = true;
  // True once the OLD plane (0x10) holds a valid previous displayed frame, so a
  // differential partial has a real baseline to diff against (no ghosting).
  // Cleared after grayscale (which overwrites the planes) so the next B/W is full.
  bool _oldPlaneValid = false;
  // AA CDI select: the first grayscale refresh after init sends the border-driving
  // CDI (0x29); later ones the border-holding CDI (0xA9), per the vendor reference.
  bool _grayRefreshedOnce = false;

  // Async split state (see Uc8279Driver for the contract).
  bool _pendingRefresh = false;
  bool _pendingTurnOff = false;
  bool _pendingPartial = false;  // this refresh used the PTIN/PTOUT partial path
};

PanelDriver& uc8179Driver();

}  // namespace freeink
