#pragma once

// FreeInk SDK — display facade.
//
// FreeInkDisplay is the stable, hardware-independent display API the firmware
// calls. It owns the framebuffer(s) and geometry and delegates every panel
// operation to a PanelDriver selected at begin(). Drivers per controller
// (SSD1677, UC8253-X3, ED2208-M5, UC8253-Murphy) live in standalone files and
// are linked per build; X3 and X4 are both linked in the generic ESP32-C3 bin
// and chosen at runtime (setDisplayX3()), so one binary drives both.
//
// The public surface below is byte-compatible with the EInkDisplay API, so
// firmware builds unchanged through the EInkDisplay.h alias.

#include <Arduino.h>
#include <BoardConfig.h>  // device flags (sizes the framebuffer for the largest panel)
#include <SPI.h>

#include "../src/bus/EpdBus.h"

namespace freeink {

class PanelDriver;

class FreeInkDisplay {
 public:
  FreeInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy);
  ~FreeInkDisplay() = default;

  // Refresh modes (public contract — full / balanced-half / fast).
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  // Select panel geometry/controller before begin().
  void setDisplayX3();
  void setDisplayM5PaperColor();

  // M5 PaperColor: run the next refresh's OTP waveform to completion (one-shot).
  void requestCompleteWaveformNextRefresh();

  // M5 PaperColor: interrupted-refresh cutoff (ms). The cut freezes the gate
  void setFastRefreshCutoffMs(uint16_t ms);
  uint16_t fastRefreshCutoffMs() const;

  void begin();

  // Legacy compile-time dimensions kept for compatibility.
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;
  static constexpr uint16_t X3_DISPLAY_WIDTH = 792;
  static constexpr uint16_t X3_DISPLAY_HEIGHT = 528;
  static constexpr uint16_t X3_DISPLAY_WIDTH_BYTES = X3_DISPLAY_WIDTH / 8;
  static constexpr uint32_t X3_BUFFER_SIZE = X3_DISPLAY_WIDTH_BYTES * X3_DISPLAY_HEIGHT;
  // Sized to the largest panel in the build — derived from the device set in the
  // registry (no device names here). One binary holds whichever panel is
  // runtime-selected; a single-device build gets exactly that panel's size.
  static constexpr uint32_t MAX_BUFFER_SIZE = BoardConfig::MAX_FRAMEBUFFER_BYTES;

  // Runtime dimensions
  uint16_t getDisplayWidth() const { return displayWidth; }
  uint16_t getDisplayHeight() const { return displayHeight; }
  uint16_t getDisplayWidthBytes() const { return displayWidthBytes; }
  uint32_t getBufferSize() const { return bufferSize; }

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem = false) const;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void swapBuffers();
#endif
  void setFramebuffer(const uint8_t* bwBuffer) const;

  // X3 grayscale preconditioning settle pass, windowed to the gray region in
  // physical panel coordinates; call after the BW base frame is displayed and
  // before the grayscale planes are written. The no-arg overload settles the
  // full frame. No-op on panels that do not need it. See
  // Uc8253X3Driver::preconditionGrayscale.
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows. X3 uses the OEM differential base waveform; other panels display
  // normally with `fallback` mode. See PanelDriver::displayGrayscaleBase.
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);
  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  enum GrayPlane { GRAY_PLANE_LSB, GRAY_PLANE_MSB };
  void writeGrayscalePlaneStrip(GrayPlane plane, const uint8_t* rows, uint16_t yStart, uint16_t numRows);
  bool supportsStripGrayscale() const;
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
#endif

  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  void displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen = false);
  void displayGrayBuffer(bool turnOffScreen = false, const unsigned char* lut = nullptr, bool factoryMode = false);

  void refreshDisplay(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);

  // Hint the X3 policy to run a one-shot full resync on next update.
  void requestResync(uint8_t settlePasses = 0);
  void skipInitialResync();

  // debug function
  void grayscaleRevert();

  // LUT control
  void setCustomLUT(bool enabled, const unsigned char* lutData = nullptr);

  // Power management
  void deepSleep();

  // Optional hooks fired around long BUSY waits (~0.3-2 s per refresh), so host
  // firmware can apply its own power policy (e.g. reduce the CPU clock) for the
  // wait window. Forwards to the bus, which owns every driver's busy-polling.
  // See EpdBus::setBusyWaitHooks for firing semantics.
  void setBusyWaitHooks(void (*beginHook)(), void (*endHook)()) { _bus.setBusyWaitHooks(beginHook, endHook); }

  // Optional slice hook replacing the BUSY poll delay once a wait has proven
  // long, so host firmware can sleep through the refresh instead of polling.
  // See EpdBus::setBusyWaitSliceHook for the contract.
  void setBusyWaitSliceHook(bool (*sliceHook)(int8_t busyPin, uint8_t busyLevel)) {
    _bus.setBusyWaitSliceHook(sliceHook);
  }

  // Access to frame buffer
  uint8_t* getFrameBuffer() const { return frameBuffer; }

  // Save the current framebuffer to a PBM file (desktop/test builds only)
  void saveFrameBufferAsPBM(const char* filename);

 private:
  void selectDriver();

  EpdPins _pins;
  EpdBus _bus;
  PanelDriver* _driver = nullptr;

  enum class PanelSel : uint8_t { X4, X3, M5 };
  PanelSel _panelSel = PanelSel::X4;

  // Runtime display geometry (seeded from the driver at begin()).
  uint16_t displayWidth = DISPLAY_WIDTH;
  uint16_t displayHeight = DISPLAY_HEIGHT;
  uint16_t displayWidthBytes = DISPLAY_WIDTH_BYTES;
  uint32_t bufferSize = BUFFER_SIZE;

  // Frame buffer (facade-owned). Static DRAM by default; PSRAM heap on devices
  // with tight DRAM but PSRAM (see FREEINK_FB_PSRAM in BoardConfig.h), allocated
  // in begin().
#if FREEINK_FB_PSRAM
  uint8_t* frameBuffer0 = nullptr;
#else
  uint8_t frameBuffer0[MAX_BUFFER_SIZE];
#endif
  uint8_t* frameBuffer;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
#if FREEINK_FB_PSRAM
  uint8_t* frameBuffer1 = nullptr;
#else
  uint8_t frameBuffer1[MAX_BUFFER_SIZE];
#endif
  uint8_t* frameBufferActive;
#endif
};

}  // namespace freeink