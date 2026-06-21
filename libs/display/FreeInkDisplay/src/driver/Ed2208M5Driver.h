#pragma once

// ED2208 panel driver — M5Stack PaperColor (Spectra 6 color e-paper, ESP32-S3).
//
// WHY THIS DRIVER EXISTS / how it gets reading-speed refreshes:
// The PaperColor is natively a SIX-COLOR, full-refresh panel — a complete OTP
// waveform takes ~15 s, which is unusable for reading. To get reading-compatible
// speeds, this driver INTERRUPTS the refresh path at ~340 ms. The colors settle
// in order, with white settling LAST, so cutting off early leaves the panel
// black or yellow (depending on the inversion/polarity in use) instead of white.
// We exploit that: by choosing the polarity, the interrupted refresh produces a
// usable high-contrast monochrome image far faster than the full 15 s waveform.
// Full color (or a true white background) requires running the complete
// waveform — see requestCompleteWaveformNextRefresh().
//
// DC BALANCE: an interrupted waveform is not charge-neutral — every fast
// refresh leaves a small net DC bias on the pixels, and over hours of
// interrupted-only operation the panel visibly darkens and colors fade. Only
// the complete waveform is DC-balanced, so consumers must schedule one
// periodically (~hourly) via requestCompleteWaveformNextRefresh().
//
// For users who prefer the vendor path over this speed hack, M5OfficialDriver
// wraps M5GFX/M5Unified instead (opt in with -DFREEINK_M5_OFFICIAL=1).
//
// Selection: only linked when -DFREEINK_DRIVER_ED2208 (set by the M5 board env).

#include "PanelDriver.h"

namespace freeink {

class Ed2208M5Driver : public PanelDriver {
 public:
  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  PanelGeometry geometry() const override;
  int8_t spiMiso() const override;  // M5 shares the SD MISO
  int8_t coCs() const override;     // SD CS held high during panel transactions

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

  void requestCompleteWaveformNextRefresh() override { _completeNextRefresh = true; }

  // Interrupted-refresh cutoff. The cut freezes the gate scan mid-frame: rows
  // already scanned that frame got one more drive step, so the scan position at
  // the cut shows as a hard band across the panel. Tunable so the cutoff can be
  // swept on a live panel until the seam lands at a frame boundary (row 0).
  void setFastRefreshCutoffMs(uint16_t ms) override;
  uint16_t fastRefreshCutoffMs() const override;

 private:
  void enablePmicPower();
  void initController(EpdBus& bus);
  void waitBusy(EpdBus& bus);
  void writeFrame(EpdBus& bus, const uint8_t* fb);
  void setPartialWindow(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  void powerOn(EpdBus& bus);
  void powerOff(EpdBus& bus);
  void interruptRefresh(EpdBus& bus);
  bool getDirtyWindow(const uint8_t* fb, uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h) const;
  void refresh(EpdBus& bus, uint16_t dirtyX, uint16_t dirtyY, uint16_t dirtyW, uint16_t dirtyH);

  // Landscape framebuffer geometry (physical panel is 400x600; app draws 600x400).
  static constexpr uint16_t LOGICAL_W = 600;
  static constexpr uint16_t LOGICAL_H = 400;
  static constexpr uint16_t LOGICAL_WB = LOGICAL_W / 8;                        // 75
  static constexpr uint32_t LOGICAL_BUF = static_cast<uint32_t>(LOGICAL_WB) * LOGICAL_H;  // 30000

  bool _panelPowerOn = false;
  bool _completeNextRefresh = false;
  uint16_t _cutoffMs = 0;  // 0 -> REFRESH_CUTOFF_MS default (set in .cpp)
  bool _lastFrameValid = false;
  uint8_t _prevFrame[LOGICAL_BUF];  // previous frame, for the dirty-window diff
};

PanelDriver& ed2208M5Driver();

}  // namespace freeink
