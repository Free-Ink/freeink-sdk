#pragma once

// Raw-parallel EPD driver over EPDiy (libs/display/epdiy, LGPL-3.0 submodule).
//
// Same panel class as LgfxEpdDriver (no on-glass controller; the MCU clocks
// every row through its LCD peripheral and an external PMIC makes the rails),
// for chips LovyanGFX does not drive, such as the ESP32-S31.
//
// Selection: compiled when FREEINK_DRIVER_EPDIY=1 (derived from the device).
// The board must supply `const EpdiyConfig& yourConfig();` in namespace freeink
// and build with -DFREEINK_EPDIY_CONFIG=yourConfig.

#include <EpdiyConfig.h>

#include "PanelDriver.h"

#if FREEINK_DRIVER_EPDIY

namespace freeink {

class EpdiyDriver : public PanelDriver {
 public:
  explicit EpdiyDriver(const EpdiyConfig& cfg);

  const char* controllerName() const override { return "EPDiy"; }
  uint32_t spiHz() const override { return 0; }  // EPDiy owns the LCD bus
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  bool usesExternalBus() const override { return true; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

  // Overlay masks onto the B/W base the host displays first: LSB = dark gray,
  // MSB-only = light gray. Whole planes or strips.
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, true, false, false};
  }
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;

 private:
  bool update(enum EpdDrawMode mode, bool full);
  int temperature() const;

  const EpdiyConfig& _cfg;
  EpdiyHighlevelState _hl{};
  bool _ready = false;
  bool _panelKnown = false;  // false until a clear has put the glass in a known state
  uint8_t* _lsb = nullptr;
  uint8_t* _msb = nullptr;
  enum EpdDrawMode _lastBaseMode = MODE_GL16;
};

PanelDriver& epdiyDriver();

}  // namespace freeink

#endif  // FREEINK_DRIVER_EPDIY
