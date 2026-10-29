#pragma once

// Onyx Picco SE0400NQW47 panel — the controller the stock firmware calls
// "SSD2677" (identified by CMD 0x70 -> 07 01 01). Its command set is
// UltraChip-style, not SSD1677: panel setting 0x00, window 0x83, data 0x10, one
// 535-byte LUT via 0x20, power on/off 0x04/0x02, refresh 0x12, deep sleep 0x07.
// BUSY is active-LOW.
//
// Each frame goes out as 2 bits per pixel through a single 0x10 burst: the
// previous frame in the high bit, the new frame in the low bit; the LUT picks
// the transition. Init list, LUTs and bit packing are from the stock firmware
// (build 2912). Grayscale is the stock overlay AA mode (slot-5 LUT): the LSB
// and MSB masks are packed as the low/high bit of each pixel. Stock's image mode
// (slot 4) uses an unrecovered encoding, so absolute grayscale is not advertised.
//
// The newer Seeed Sticky carries the same controller with OTP waveforms: its
// stock firmware (v1.2.0) uploads no LUT but latches an OTP waveform before each
// frame (0xE0 0x12, 0xE6 code, 0xA5), sends full refreshes with old == new, and
// keeps the rails on between fast refreshes. Its images use stock's OTP gray4
// waveform (one pass); text AA uses the Picco's uploaded overlay waveform.
//
// Selected by FreeInkDisplay when piccoProbePanel() reports PiccoSe0400nqw47, or
// on a Sticky when stickyHasSsd2677().

#include <BoardConfig.h>

#include "PanelDriver.h"

namespace freeink {

class Ssd2677Driver : public PanelDriver {
 public:
  const char* controllerName() const override { return "SSD2677"; }
  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override {
    return BoardConfig::ACTIVE.board == BoardConfig::Board::Sticky ? BusyPolarity::ActiveLowReady
                                                                   : BusyPolarity::ActiveLow;
  }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void powerOffBetweenFrames(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

  // Overlay AA with the Picco's uploaded gray LUT on both boards. Sticky also
  // does absolute four-level images in one pass on its OTP gray waveform
  // (stock gray4), so no B/W base is shown first. The Picco's controller hangs
  // in BUSY on that latch (tried on hardware), so it stays overlay-only.
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode == GrayscaleMode::Overlay)
      return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, false, false, false};
    if (mode == GrayscaleMode::Absolute && BoardConfig::ACTIVE.board == BoardConfig::Board::Sticky)
      return {GrayscaleEncoding::AbsolutePlanes, GrayscaleBase::Combined, false, false, false};
    return {};
  }
  void beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback, bool turnOff) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;

 private:
  void initPanel(EpdBus& bus);
  void latchOtpWaveform(EpdBus& bus, uint8_t code);
  void sendPacked(EpdBus& bus, const uint8_t* hi, const uint8_t* lo);
  void powerAndRefresh(EpdBus& bus, bool turnOff);
  void refreshWith(EpdBus& bus, const unsigned char* lut, bool turnOff);

  bool _needsPowerOn = true;
  bool _needsInitialFull = true;
  uint8_t _latchedWaveform = 0;  // Sticky: OTP waveform code currently latched (0 = none)
  bool _absoluteGray = false;  // the current gray pass began as absolute (OTP gray4)
  uint8_t* _lsb = nullptr;  // retained LSB mask: the renderer reuses its buffer for the MSB pass
  uint8_t* _old = nullptr;  // retained old plane (last displayed frame); host is single-buffer
                            // so `prev` is always null — mirror stock's obj+0x1020, seeded white
};

PanelDriver& ssd2677Driver();

}  // namespace freeink
