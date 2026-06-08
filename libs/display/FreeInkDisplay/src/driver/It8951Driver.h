#pragma once

// IT8951E controller driver — M5Paper v1.1 (ED047TC1, 540x960, 16-gray, ESP32).
//
// Unlike the panel-direct controllers (SSD1677/UC8253), the IT8951E is a timing
// controller with its own framebuffer SRAM. The host doesn't push waveforms: it
// loads an image into the controller's memory, then asks it to display a region
// with one of the IT8951's built-in waveform modes (INIT/GC16/DU/A2...). The
// IT8951 owns all LUT/VCOM/temperature handling internally.
//
// The protocol is 16-bit-word SPI with preamble words and MISO reads (device
// info, register reads, HRDY flow control) — a poor fit for the byte-oriented,
// write-only EpdBus — so this driver reports usesExternalBus() == true and drives
// its own SPIClass end to end (pins from BoardConfig::ACTIVE.display; MISO,
// rotation, VCOM, and clock from the injectable It8951Config).
//
// Selection: linked only when -DFREEINK_DRIVER_IT8951 (M5Paper env).

#include <SPI.h>

#include "PanelDriver.h"

namespace freeink {

// cfg.rotation sentinel: pick 0° or 90° at begin() from the panel's reported
// orientation so the landscape framebuffer lands upright on the portrait panel.
constexpr uint16_t IT8951_ROTATE_AUTO = 0xFF;

struct It8951Config {
  int8_t miso;        // SPI MISO (IT8951 register / device-info reads)
  uint32_t spiHz;     // SPI clock
  uint16_t rotation;  // LD_IMG rotation 0/1/2/3 = 0/90/180/270°, or IT8951_ROTATE_AUTO
  uint16_t vcomMv;    // VCOM magnitude in mV (e.g. 2300 = -2.30 V); 0 = keep panel OTP
  uint8_t fullMode;   // waveform mode for RefreshMode::Full (GC16 = 2)
  uint8_t halfMode;   // waveform mode for RefreshMode::Half (GL16 = 3)
  uint8_t fastMode;   // waveform mode for RefreshMode::Fast (DU = 1)
  uint32_t imgBufFallbackAddr;  // used only if GET_DEV_INFO returns no buffer address
};

const It8951Config& it8951DefaultConfig();

class It8951Driver : public PanelDriver {
 public:
  explicit It8951Driver(const It8951Config& cfg = it8951DefaultConfig());

  uint32_t spiHz() const override { return 0; }                                    // owns its own SPI
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }  // HRDY high = ready
  bool usesExternalBus() const override { return true; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

 private:
  // --- low-level SPI framing ---
  void waitReady();                                    // block until HRDY high
  void writeWord(uint16_t preamble, uint16_t value);   // one preamble+word CS-framed write
  void writeCommand(uint16_t cmd);
  void writeData(uint16_t data);
  uint16_t readData();
  void readWords(uint16_t* buf, uint16_t count);       // bulk read in one CS-low frame
  void writeReg(uint16_t reg, uint16_t value);
  uint16_t readReg(uint16_t reg);

  // --- IT8951 operations ---
  void systemRun();
  void getDeviceInfo();
  void setTargetMemoryAddr(uint32_t addr);
  void setVcom(uint16_t mv);
  void loadImageFull(const uint8_t* fb);  // expand 1bpp framebuffer -> 4bpp into controller SRAM
  void displayArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode);
  void waitDisplayReady();                // poll LUT-busy register

  const It8951Config& _cfg;
  // Reference to the Arduino global SPI bus (VSPI on ESP32) — the SAME object the
  // SDCardManager uses. On M5Paper the SD card and the IT8951 share one physical
  // SPI bus; two separate SPIClass instances bound to one VSPI peripheral corrupt
  // each other's transfers, so both must drive the one global bus object (manual
  // CS selects the device).
  SPIClass& _spi;

  uint16_t _fbW;   // landscape framebuffer width (960)
  uint16_t _fbH;   // landscape framebuffer height (540)
  uint16_t _fbWb;  // framebuffer width bytes (120)

  // Resolved at begin().
  int8_t _sclk = -1, _mosi = -1, _miso = -1, _cs = -1, _busy = -1, _pwrEn = -1, _sdCs = -1;
  uint16_t _panelW = 0, _panelH = 0;
  uint32_t _imgBufAddr = 0;
  uint16_t _rotation = 0;
  bool _running = false;
};

PanelDriver& it8951Driver();

}  // namespace freeink
