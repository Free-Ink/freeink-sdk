#pragma once

// UC8253 panel driver — Xteink X3 (792x528 B/W + 4- and 16-level grayscale).
// Selected at runtime via FreeInkDisplay::setDisplayX3() so one binary drives
// both X3 and X4.
//
// Refresh & grayscale modes:
//   FAST -> `_fast` differential LUTs (hardware N2OCP=1 copies DTM2 -> DTM1);
//           windowed updates use PTL (CDI 0xA9) with explicit sub-rect sync.
//   HALF -> `_half` scrub LUTs (WW==BW, WB==BB: history-independent of DTM1).
//   FULL -> `_full` OEM bank from a white DTM1 baseline + post-full settle.
//   4-gray -> `_gc` 5-frame nudge (Overlay/Absolute) or `directGray` (Direct).
//   16-gray -> cross-dissolve or `bipolarScrub` base + 2-pass coarse/fine LUTs;
//              `beginGray16Cleanup`/`finishGray16Cleanup` stages `0xFFFE` in
//              DTM1 so the next Fast B/W or 16-gray turn cross-dissolves in 1 pass.
//
// X3TwoPhase BUSY; 16 MHz default SPI; 1.2 s post-settle idle HV power-off.

#include <Uc8253X3Config.h>

#include "PanelDriver.h"

namespace freeink {

class Uc8253X3Driver : public PanelDriver {
 public:
  static constexpr uint32_t kIdlePowerOffMs = 1200;

  const char* controllerName() const override { return "UC8253"; }
  explicit Uc8253X3Driver(const Uc8253X3Config& cfg = uc8253X3DefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::X3TwoPhase; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  bool supportsBetweenFramePowerOff() const override { return true; }
  void powerOffBetweenFrames(EpdBus& bus) override;
  void controllerIdle(EpdBus& bus) override { powerOffBetweenFrames(bus); }
  bool prewarmPowerOn(EpdBus& bus) override;
  bool hasIdlePowerWork() const override { return (_isScreenOn || _powerOnPending) && _pendingDrf == PendingDrf::None; }
  bool serviceIdlePower(EpdBus& bus) override;
  void setBackgroundHint(bool darkBackground) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y, uint16_t w,
                     uint16_t h, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  void beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback, bool turnOff) override;
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;
  void preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode == GrayscaleMode::Direct && !_cfg.directGray) return {};
    if (mode == GrayscaleMode::Absolute || mode == GrayscaleMode::Direct)
      return {GrayscaleEncoding::AbsolutePlanes,
              mode == GrayscaleMode::Direct ? GrayscaleBase::Combined : GrayscaleBase::Separate, true, false, false};
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, true, false, false};
  }
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  bool supportsGrayscaleGateStream() const override { return !_directGrayPass && !_directGrayOnPanel; }
  bool writeGrayscalePlaneGateStream(EpdBus& bus, GrayPlane plane, void* ctx,
                                     size_t (*nextChunk)(void* ctx, const uint8_t** outChunk)) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  bool supportsGray16Planes() const override {
    return hasLutBank(_cfg.lut16.preBwMid) && hasLutBank(_cfg.lut16.pass1Coarse) && hasLutBank(_cfg.lut16.pass2Fine);
  }
  bool supportsDeferredGray16Base() const override { return supportsGray16Planes(); }
  void displayGray16Base(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff = false) override;
  bool displayGray16BaseStart(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff = false) override;
  void displayGray16Pass1(EpdBus& bus) override;
  bool displayGray16Pass1Start(EpdBus& bus) override;
  void displayGray16Pass2(EpdBus& bus, bool turnOff) override;
  bool displayGray16Pass2Start(EpdBus& bus, bool turnOff) override;
  void beginGray16Cleanup(EpdBus& bus, const uint8_t* bw) override;
  void finishGray16Cleanup(EpdBus& bus) override;
  bool isGray16CleanupActive() const override { return _gray16CleanupActive; }
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;
  void grayscaleRevert(EpdBus& bus, const uint8_t* fb) override;

  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;

 private:
  enum class PendingDrf : uint8_t { None, Gray16Base, Gray16Pass1, Gray16Pass2, Gray16Pass2TurnOff };

  static constexpr bool hasLutBank(const Uc8253LutBank& bank) {
    return bank.vcom != nullptr && bank.ww != nullptr && bank.bw != nullptr && bank.wb != nullptr && bank.bb != nullptr;
  }
  void initController(EpdBus& bus);
  void loadBank(EpdBus& bus, const Uc8253LutBank& bank);
  void loadBankCdi(EpdBus& bus, uint8_t cdi0, uint8_t cdi1, const Uc8253LutBank& bank);
  void finishPendingPowerOn(EpdBus& bus);
  void ensurePowerOn(EpdBus& bus);
  void triggerRefresh(EpdBus& bus, bool turnOff, const char* label = " X3_DRF");
  void startAsyncRefresh(EpdBus& bus, PendingDrf kind);
  void drainPendingRefresh(EpdBus& bus);
  bool displayGrayscaleBaseImpl(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff, bool gray16Pass,
                                bool allowDefer);
  bool displayGray16Pass1Impl(EpdBus& bus, bool allowDefer);
  bool displayGray16Pass2Impl(EpdBus& bus, bool turnOff, bool allowDefer);
  void grayWindowIn(EpdBus& bus);

  const Uc8253X3Config& _cfg;

  uint16_t _w;
  uint16_t _h;
  uint16_t _wb;
  uint32_t _bufferSize;

  bool _isScreenOn = false;
  bool _powerOnPending = false;
  bool _powerOnSawLow = false;
  bool _pendingDrfSawLow = false;
  bool _hasPresentedSinceInit = false;
  bool _darkBackground = false;
  bool _polarityDirty = false;
  bool _redRamSynced = false;
  bool _inGrayscaleMode = false;
  bool _absoluteInput = false;
  bool _directGrayPass = false;
  bool _directGrayOnPanel = false;
  bool _gray16CleanupActive = false;
  PendingDrf _pendingDrf = PendingDrf::None;
  uint8_t _initialFullSyncsRemaining = 0;
  bool _forceFullSyncNext = false;
  uint8_t _forcedConditionPassesNext = 0;
  uint32_t _lastActivityMs = 0;
  uint32_t _powerOnStartMs = 0;
  struct GrayState {
    bool lastBaseWasPartial = false;
    bool lsbValid = false;
  } _grayState;
};

PanelDriver& uc8253X3Driver();

}  // namespace freeink
