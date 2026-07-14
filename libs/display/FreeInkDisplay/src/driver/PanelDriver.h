#pragma once

// FreeInk SDK — panel driver interface.
//
// One PanelDriver implementation exists per display controller (SSD1677,
// UC8253-X3, ED2208-M5, UC8253-Murphy). The FreeInkDisplay facade owns the
// framebuffer and selects a driver at begin(); the driver owns all
// controller-specific register sequences, LUTs, timing, and cross-call state.
//
// The facade does all framebuffer composition (clear/draw) itself and passes
// raw buffer pointers in here — drivers only touch hardware. `prev` is the
// previous frame in dual-buffer mode, or nullptr in single-buffer mode (the
// controller's own RAM holds the previous frame).

#include <Arduino.h>

#include "../bus/EpdBus.h"

namespace freeink {

enum class RefreshMode : uint8_t { Full, Half, Fast };
enum class GrayPlane : uint8_t { Lsb, Msb };

struct PanelGeometry {
  uint16_t width;
  uint16_t height;
  uint16_t widthBytes;
  uint32_t bufferSize;
};

class PanelDriver {
 public:
  virtual ~PanelDriver() = default;

  // --- bus configuration (consumed by the facade before begin()) ---
  virtual uint32_t spiHz() const = 0;
  virtual BusyPolarity busyPolarity() const = 0;
  virtual PanelGeometry geometry() const = 0;
  virtual int8_t spiMiso() const { return -1; }  // SSD1677 uses none; M5 shares MISO
  virtual int8_t coCs() const { return -1; }      // co-resident SPI CS to hold high (M5 SD)

  // True for drivers backed by an external library that manages its own SPI /
  // display hardware (e.g. M5GFX, EPD_Painter). When true the facade does NOT
  // bring up its EpdBus — the driver owns the panel end to end.
  virtual bool usesExternalBus() const { return false; }

  // --- lifecycle ---
  virtual void begin(EpdBus& bus) = 0;
  virtual void deepSleep(EpdBus& bus) = 0;

  // --- core paint path (load RAM + refresh) ---
  virtual void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) = 0;
  virtual void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y, uint16_t w,
                             uint16_t h, bool turnOff) {
    display(bus, fb, prev, RefreshMode::Fast, turnOff);
  }

  // Non-blocking variant: load RAM and start the panel waveform, then return
  // without waiting on BUSY. The panel refreshes from its own RAM copy, so the
  // caller may redraw `fb` immediately; the facade polls the BUSY pin and
  // guards against issuing another operation until the refresh completes.
  // Default falls back to the blocking display() so drivers gain async
  // support one at a time without breaking correctness.
  virtual void displayAsync(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode) {
    display(bus, fb, prev, mode, false);
  }
  // True when displayAsync() actually returns while the panel refreshes. The
  // facade uses this to skip async bookkeeping on drivers that fall back to
  // the blocking display() — otherwise the post-refresh BUSY poll can spin a
  // full edge-detect timeout against an already-idle panel (X3TwoPhase: 1 s).
  virtual bool supportsAsyncDisplay() const { return false; }

  // --- grayscale (dual-plane LSB/MSB) ---
  virtual bool supportsStripGrayscale() const { return false; }
  // Display `fb` as the base frame for a grayscale overlay that follows.
  // X3 runs the OEM pipeline (the "AA-pre-BW(mid)" bank as a differential
  // base update with calibrated drives); panels without a dedicated base
  // waveform fall back to a plain display() with `fallback` mode, preserving
  // their previous behavior.
  virtual void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
    display(bus, fb, nullptr, fallback, turnOff);
  }

  // Grayscale preconditioning settle pass (OEM X3 "AA-pre-BW(mid)"), windowed
  // to the panel rect [x, x+w) x [y, y+h) like the OEM's PTL usage; fire after
  // the BW base frame is displayed, before grayscale planes are written.
  // Default no-op for panels whose grayscale needs no conditioning.
  virtual void preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    (void)bus; (void)x; (void)y; (void)w; (void)h;
  }
  virtual void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) { (void)bus; (void)lsb; }
  virtual void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) { (void)bus; (void)msb; }
  virtual void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                        uint16_t numRows) {
    (void)bus; (void)plane; (void)rows; (void)yStart; (void)numRows;
  }
  virtual void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) {
    (void)lut;
    (void)factoryMode;
    display(bus, fb, nullptr, RefreshMode::Fast, turnOff);
  }
  virtual void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) { (void)bus; (void)bw; }

  // --- optional, controller-specific hooks (no-op by default) ---
  virtual void requestResync(uint8_t settlePasses) { (void)settlePasses; }
  virtual void skipInitialResync() {}
  virtual void requestCompleteWaveformNextRefresh() {}
  // Interrupted-refresh cutoff tuning (ED2208: where the gate scan freezes).
  virtual void setFastRefreshCutoffMs(uint16_t ms) { (void)ms; }
  virtual uint16_t fastRefreshCutoffMs() const { return 0; }
  virtual void grayscaleRevert(EpdBus& bus, const uint8_t* fb) { (void)bus; (void)fb; }
  virtual void setCustomLut(EpdBus& bus, bool enabled, const unsigned char* data) { (void)bus; (void)enabled; (void)data; }
};

}  // namespace freeink
