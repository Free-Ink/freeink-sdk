#pragma once

// Raw-parallel EPD driver via the EPDiy core (libs/display/Epdiy, LGPL-3.0).
//
// For panels with no on-glass controller: the S31 LCD peripheral clocks every
// row from a 4-bpp framebuffer and the board's PMIC makes the rails. EPDiy owns
// that bus, so usesExternalBus() == true and EpdBus is never brought up.
//
// The facade's 1-bpp frame (bit set = white) is expanded into EPDiy's 4-bpp
// front buffer. EPDiy keeps the previous frame itself, so `prev` is unused.
// Full clears the glass and draws every pixel from white; Half drives every
// pixel with GC16 (unchanged ones included, which cleans DU residue); Fast uses
// DU on changed pixels unless it changes over a quarter of the screen, which
// then runs GC16 on the changed pixels because DU leaves a ghost of the old screen. Grayscale overlays
// (text anti-aliasing, images) are painted into the same front buffer as two
// extra grey levels and drawn with GC16 over the B/W base.
//
// Selection: linked only when FREEINK_DRIVER_EPDIY (derived from
// FREEINK_DEVICE_METALIO_EINK4_PLUS), which also needs the Epdiy and
// MetalioPlusBoard libraries in lib_deps. The board glue, panel definition and
// VCOM are currently fixed to the Metalio E-Ink4-Plus; a second EPDiy board
// would inject them as a config, as LgfxEpdDriver does with LgfxEpdConfig.

#include "PanelDriver.h"

namespace freeink {

class EpdiyDriver : public PanelDriver {
 public:
  uint32_t spiHz() const override { return 0; }  // EPDiy owns the bus
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  bool usesExternalBus() const override { return true; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, true, false, false};
  }
  // Stages the base in the front buffer without drawing; displayGray() then
  // draws the base and the grey planes in one waveform.
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;

  // The next frame drives every pixel with GC16 (as Half).
  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;
};

PanelDriver& epdiyDriver();

}  // namespace freeink
