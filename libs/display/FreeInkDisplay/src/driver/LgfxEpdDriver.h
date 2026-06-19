#pragma once

// Raw-parallel EPD driver via LovyanGFX (bundled in M5GFX).
//
// For panels with NO on-glass controller/RAM — the MCU clocks every row/column
// over the ESP32-S3 LCD (i80) peripheral and an external PMIC generates the
// waveform rails. The LilyGo T5 S3 4.7" (ED047TC1, 960x540, 16-gray) is the
// reference. This is a different class from the SPI single-chip controllers
// (SSD1677/UC8253/ED2208); it can't use EpdBus, so usesExternalBus() == true and
// LovyanGFX's Panel_EPD/Bus_EPD own the bus (exactly like M5OfficialDriver wraps
// M5GFX for the PaperColor).
//
// The SDK owns the generic Panel_EPD/Bus_EPD wiring; the board owns only its
// power topology (PMIC + any IO-expander control lines), injected through
// LgfxEpdPowerHooks. Geometry comes from BoardProfile (ACTIVE.displayWidth/Height),
// like every other driver.
//
// Selection: linked only when -DFREEINK_DRIVER_LGFX_EPD=1 (derived from
// FREEINK_DEVICE_LILYGO), which also pulls m5stack/M5GFX into lib_deps. The board
// MUST supply a config via -DFREEINK_LGFX_EPD_CONFIG=yourConfig.

#include <LgfxEpdConfig.h>

#include "PanelDriver.h"

namespace freeink {

class LgfxEpdDriver : public PanelDriver {
 public:
  explicit LgfxEpdDriver(const LgfxEpdConfig& cfg);

  uint32_t spiHz() const override { return 0; }  // LovyanGFX owns the bus
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  bool usesExternalBus() const override { return true; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

  // 16-gray path: the facade streams LSB/MSB 1-bpp planes (whole or in strips);
  // displayGray combines them with the B/W base into the panel's 8-bit gray canvas.
  bool supportsStripGrayscale() const override { return true; }
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;

 private:
  const LgfxEpdConfig& _cfg;
};

PanelDriver& lgfxEpdDriver();

}  // namespace freeink
