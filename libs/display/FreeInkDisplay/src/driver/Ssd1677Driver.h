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
  uint8_t borderWaveformInit;        // CMD 0x3C value written during controller init
  uint8_t halfRefreshTemp;          // temperature byte written for HALF refresh
  const unsigned char* grayLut;     // 110-byte custom LUT for grayscale display
  const unsigned char* grayRevertLut;  // 110-byte custom LUT to revert grayscale
  // Absolute Display Update Control 2 (0x22) sequence values, per refresh type.
  // 0 = use the driver's built-in X4 values (incremental, keeps the panel powered
  // between fast refreshes). A panel whose OTP waveform isn't selected by the X4
  // values supplies its vendor-published values here — they pick the waveform
  // (full vs partial/DU), load temperature, and self-cycle power. Setting these
  // makes fast refreshes use the panel's real DU waveform instead of running the
  // full waveform every time. (e.g. Sticky: full 0xF7 / fast 0xFF, from Seeed's
  // SSD1677 driver.) Ignored while a custom grayscale LUT is active.
  uint8_t fullSeqOverride = 0;  // FULL refreshes
  uint8_t fastSeqOverride = 0;  // FAST (UI) refreshes
  uint8_t halfSeqOverride = 0;  // HALF refreshes; 0 falls back to fullSeqOverride
  // Border waveform (CMD 0x3C) re-written per refresh in the seqOverride path so
  // it tracks the refresh mode (vendor parity); without this a partial/DU refresh
  // leaves the border driven dark (Sticky's black ring). 0 = keep the init value.
  // Only consulted when fullSeqOverride / fastSeqOverride are set.
  uint8_t borderWaveformFull = 0;  // FULL refreshes
  uint8_t borderWaveformFast = 0;  // FAST (UI / page-turn) refreshes
  uint8_t borderWaveformHalf = 0;  // HALF refreshes; 0 falls back to borderWaveformFull
  // Border waveform while a custom (grayscale/revert) LUT is loaded. A follow-LUT
  // border value (0x0X) makes the border track a row of the loaded LUT; under the
  // grayscale LUT that drives the border black on every AA refresh. 0x80 (VCOM)
  // holds it undriven at a defined potential. 0 = leave the register untouched.
  uint8_t borderWaveformGray = 0;
  // Power the rails up in a separate activation before a custom-LUT (grayscale/
  // revert) refresh. Boards whose vendor sequences self-power-down after every
  // refresh (Sticky: fast 0xFF) otherwise fold CLOCK_ON|ANALOG_ON into the same
  // activation as the gray waveform; the LUT's 1-frame phases then run while the
  // booster is still ramping and under-drive the grays (lighter AA, mid-grays
  // collapsing toward B/W). The X4 keeps the panel powered between fast
  // refreshes, so it never needs this and keeps stock behavior.
  bool grayPowerUpFirst = false;
};

// Standard config (Xteink X4 / GDEQ0426T82). Panel mounting (mirror/180°) is NOT
// a config field — it comes from BoardProfile.orientation so any board injects it
// uniformly. -DFREEINK_DISPLAY_FLIPPED is an alias for mirrorY.
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
  void displayAsync(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode) override;
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
  // async: fire MASTER_ACTIVATION and return without waiting on BUSY.
  void refresh(EpdBus& bus, RefreshMode mode, bool turnOff, bool async = false);
  // Blocking CLOCK_ON|ANALOG_ON activation; no-op when already powered.
  void powerOn(EpdBus& bus);
  void displayImpl(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff, bool async);

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
  // First paint after begin() (boot or deep-sleep wake) must be a full refresh to
  // clear whatever is physically on the panel (e.g. the black boot screen) and set
  // a clean differential baseline. Only armed for boards whose self-powering fast
  // sequence makes _isScreenOn useless as a cold-start signal (fullSeqOverride set).
  bool _needsInitialFull = false;
};

// Singleton accessor (Meyers, zero-heap). Selects the config for the active board.
PanelDriver& ssd1677Driver();

}  // namespace freeink
