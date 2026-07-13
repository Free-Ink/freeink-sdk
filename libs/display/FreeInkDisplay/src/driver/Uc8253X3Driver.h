#pragma once

// UC8253 panel driver — Xteink X3 (792x528 B/W + grayscale). Ported from the
// community-sdk `main` lineage (the production X3 implementation CrossPoint
// ships). Selected at runtime via FreeInkDisplay::setDisplayX3() so one binary
// drives both X3 and X4.
//
// Three-tier differential refresh, all single-phase 1-bpp:
//   FAST -> `_fast` turbo LUTs (DTM1 holds prev frame, diffs against it)
//   HALF -> `_half` scrub LUTs (WW==BW, WB==BB: drive to target ignoring DTM1)
//   FULL -> `_full` OEM bank from a white DTM1 baseline + post-full settle pass
// Grayscale: `_gc` 4-level nudge (reader AA/cover) or `_full` (factory
// absolute), reverted via the `_half` scrub bank. DTM1/DTM2 are the
// controller's old/new RAM planes; CDI (cmd 0x50) selects differential (0x29)
// vs absolute (0xA9). The recovered OEM standalone banks (factoryP1/P2) are
// NOT wired up: they need the OEM's grayscale panel init (see Uc8253X3Luts.h).
//
// X3TwoPhase BUSY; SPI clock board-overridable (default 16 MHz).

#include "PanelDriver.h"

namespace freeink {

// One UC8253 waveform bank: VCOM + the four transition LUTs.
struct Uc8253LutBank {
  const uint8_t* vcom;
  const uint8_t* ww;
  const uint8_t* bw;
  const uint8_t* wb;
  const uint8_t* bb;
};

struct Uc8253X3Config {
  Uc8253LutBank normal;  // condition-pass / settle (CDI 0xA9)
  Uc8253LutBank half;    // scrub (CDI 0xA9)
  Uc8253LutBank fast;    // turbo differential (CDI 0x29)
  Uc8253LutBank full;    // OEM full / factory (CDI 0x29)
  Uc8253LutBank gc;        // OEM 4-level grayscale nudge (CDI 0x29)
  Uc8253LutBank preBwMid;  // OEM grayscale preconditioning settle (CDI 0xA9)
  // OEM standalone grayscale banks (partial long / full short). Reference only,
  // NOT used yet: they require the OEM grayscale panel init (different PSR/PWR/
  // VCOM rails) and its DTM data framing — see the note in Uc8253X3Luts.h.
  Uc8253LutBank factoryP1;  // OEM standalone gray, long bank (stock partial, CDI 0xD7)
  Uc8253LutBank factoryP2;  // OEM standalone gray, short bank (stock full, CDI 0x97)
  uint8_t lutLen;           // bytes per LUT sent to the controller (42)
};

const Uc8253X3Config& uc8253X3DefaultConfig();

class Uc8253X3Driver : public PanelDriver {
 public:
  explicit Uc8253X3Driver(const Uc8253X3Config& cfg = uc8253X3DefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::X3TwoPhase; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  // Refresh split: displayStart fires the waveform and returns while the ~130-770 ms
  // X3 waveform runs (so the render task can overlap non-SPI work); displayFinish
  // waits BUSY out and runs the post-waveform DTM1 sync + conditioning passes.
  // display() above is exactly displayStart()+displayFinish().
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;

  bool supportsStripGrayscale() const override { return true; }
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;
  void preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;
  void grayscaleRevert(EpdBus& bus, const uint8_t* fb) override;

  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;

 private:
  void initController(EpdBus& bus);
  void loadBank(EpdBus& bus, const Uc8253LutBank& bank);
  void loadBankCdi(EpdBus& bus, uint8_t cdi0, uint8_t cdi1, const Uc8253LutBank& bank);
  void triggerRefresh(EpdBus& bus, bool turnOff);

  const Uc8253X3Config& _cfg;

  uint16_t _w;
  uint16_t _h;
  uint16_t _wb;
  uint32_t _bufferSize;

  bool _isScreenOn = false;
  bool _redRamSynced = false;
  bool _inGrayscaleMode = false;
  uint8_t _initialFullSyncsRemaining = 0;
  bool _forceFullSyncNext = false;
  uint8_t _forcedConditionPassesNext = 0;
  struct GrayState {
    bool lastBaseWasPartial = false;
    bool lsbValid = false;
  } _grayState;

  // Refresh split state: what displayStart() decided, replayed by displayFinish()
  // for the post-waveform DTM1 sync + conditioning. _pendingRefresh guards against
  // a displayFinish() with no matching displayStart(). The just-displayed frame is
  // NOT stashed here: the facade re-supplies it fresh to displayFinish() because
  // the caller may release/realloc the buffer holding it in the gap.
  bool _pendingRefresh = false;
  bool _pendingTurnOff = false;
  bool _pendingDoFullSync = false;
  bool _pendingFastMode = false;
};

PanelDriver& uc8253X3Driver();

}  // namespace freeink
