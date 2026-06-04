#pragma once

// SSD1677 panel driver — Xteink X4 and the de-link ESP32-S3 board (both drive
// an 800x480 GDEQ0426T82 over the same controller). B/W with software 2-bit
// grayscale via a custom LUT, dual-RAM (BW 0x24 / RED 0x26) differential fast
// refresh. Active-HIGH BUSY.
//
// Nothing device-specific is hardcoded in the driver: geometry and SPI clock
// come from BoardConfig, and every tunable waveform value (booster soft-start,
// scan direction, grayscale LUTs, refresh temperature) is supplied through an
// Ssd1677Config. A board reuses the generic driver by passing its own config;
// the firmware just calls the generic EInkDisplay API and gets device behavior.

#include "PanelDriver.h"

namespace freeink {

// Device-tunable SSD1677 waveform/config. A board overrides only what differs.
struct Ssd1677Config {
  uint8_t booster[5];               // booster soft-start (CMD 0x0C)
  uint8_t driverOutputScan;         // CMD 0x01 base scan byte (0x02); mirrorY ORs TB
  uint8_t halfRefreshTemp;          // temperature byte written for HALF refresh
  const unsigned char* grayLut;     // 110-byte custom LUT for grayscale display
  const unsigned char* grayRevertLut;  // 110-byte custom LUT to revert grayscale
};

// Standard config (Xteink X4 / GDEQ0426T82). Panel mounting (mirror/180°) is NOT
// a config field — it comes from BoardProfile.orientation so any board injects it
// uniformly. -DFREEINK_DISPLAY_FLIPPED stays as a back-compat alias for mirrorY.
const Ssd1677Config& ssd1677DefaultConfig();

class Ssd1677Driver : public PanelDriver {
 public:
  explicit Ssd1677Driver(const Ssd1677Config& cfg = ssd1677DefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y, uint16_t w,
                     uint16_t h, bool turnOff) override;

  bool supportsStripGrayscale() const override { return true; }
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;

  void grayscaleRevert(EpdBus& bus, const uint8_t* fb) override;
  void setCustomLut(EpdBus& bus, bool enabled, const unsigned char* data) override;

 private:
  void initController(EpdBus& bus);
  void setRamArea(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  void writeRam(EpdBus& bus, uint8_t ramCmd, const uint8_t* data, uint32_t size);
  void refresh(EpdBus& bus, RefreshMode mode, bool turnOff);

  const Ssd1677Config& _cfg;

  uint16_t _w;
  uint16_t _h;
  uint16_t _wb;
  uint32_t _bufferSize;

  // Panel mount transform from BoardProfile.orientation (mirrorX via RAM column
  // addressing in setRamArea, mirrorY via the gate-scan direction). 180° = both.
  bool _mirrorX = false;
  bool _mirrorY = false;

  bool _isScreenOn = false;
  bool _inGrayscaleMode = false;
  bool _customLutActive = false;
};

// Singleton accessor (Meyers, zero-heap). Selects the config for the active board.
PanelDriver& ssd1677Driver();

}  // namespace freeink
