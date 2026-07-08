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
  // Restore controller RAM and frameBuffer to the given BW baseline after
  // grayscale. Available in both buffer modes (CrossPoint's dual-buffer HAL
  // wraps it directly).
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  // Restore controller RAM and frameBuffer to the BW baseline after grayscale.
  // Uses frameBufferActive as the source (falls back to frameBuffer when the
  // secondary buffer has been released). Call once per page-turn after
  // displayGrayBuffer() to ensure the next BW draw targets a valid BW frame.
  void cleanupGrayscaleWithPreviousBuffer();
#endif

  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);

  // Non-blocking refresh: pushes the frame, starts the panel waveform, and
  // returns (~25 ms) while the panel refreshes on its own (~0.3-2 s). Poll
  // refreshBusy(); the framebuffer is free to redraw the moment this returns
  // (the panel refreshes from its own RAM copy). Any blocking display call
  // waits out a pending async refresh first. In single-buffer mode this costs
  // one extra frame buffer (lazily heap-allocated) holding the last-displayed
  // frame as the differential baseline; if that allocation fails it falls
  // back to the blocking path.
  void displayBufferAsync(RefreshMode mode = FAST_REFRESH);
  // True while an async refresh is still running on the panel.
  bool refreshBusy();

  // ------------------------------------------------------------------------
  // CrossPoint EInkDisplay compatibility surface.
  //
  // These preserve the exact method names CrossPoint's HalDisplay / GfxRenderer
  // call, so that firmware builds unchanged through the EInkDisplay alias. Where
  // FreeInk's driver architecture already subsumes a CrossPoint optimization
  // (X4 RED-RAM baseline, single-buffer fast differential) the shim is a thin
  // state accessor; the real behavior lives in the driver.
  // ------------------------------------------------------------------------

  // Two-call refresh split. triggerDisplay() loads RAM and fires the waveform;
  // on X3 it returns while the ~130-770 ms waveform runs so the render task can
  // overlap non-SPI work, and completeDisplay() waits it out and performs the
  // post-waveform DTM1 sync + conditioning. On X4 the refresh completes inside
  // triggerDisplay() (short waveform, RED re-seeded inline) and completeDisplay()
  // is a no-op — matching CrossPoint's behavior. The framebuffer must not be
  // overwritten between the two calls; see PanelDriver::displayStart.
  void triggerDisplay(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);
  void completeDisplay();
  // True between triggerDisplay() and completeDisplay() while a waveform is in
  // flight (X3 only; X4 completes inline so this reads false right after trigger).
  bool isRefreshPending() const { return _splitPending; }

  // Returns true (X4 only) when the controller's RED RAM holds the last-displayed
  // BW frame, i.e. a fast differential can diff against it. Always false on X3.
  // Diagnostic/advisory: FreeInk's SSD1677 driver keeps RED re-seeded after every
  // refresh, so the baseline is maintained without an explicit sync.
  bool isRedRamSynced() const { return _panelSel != PanelSel::X3 && _redRamSynced; }
  // Re-establish the X4 RED-RAM differential baseline from the displayed frame.
  // On FreeInk the SSD1677 driver already re-seeds RED after each single-buffer
  // refresh, so this only refreshes the advisory flag; kept for API parity.
  void syncRedRamFromFrameBuffer();

  // Opt in to X4 fast differential against the retained RED-RAM baseline while the
  // secondary buffer is released. FreeInk's SSD1677 driver does this safely on
  // every prev==nullptr refresh, so this is an advisory flag (no downgrade path).
  void setSingleBufferFastDiff(bool enabled) { _singleBufferFastDiff = enabled; }
  bool singleBufferFastDiff() const { return _singleBufferFastDiff; }

  // X3-only: pick the fast (community) vs accurate (OEM) grayscale LUT bank.
  // FreeInk's X3 driver currently carries the single community `_gc` bank, so
  // this stores the preference for getFastGrayscaleLut() but does not yet switch
  // banks; see Uc8253X3Driver. No effect on X4.
  void setFastGrayscaleLut(bool fast) { _fastGrayscaleLut = fast; }
  bool getFastGrayscaleLut() const { return _fastGrayscaleLut; }

  // True when the runtime-selected panel is the Xteink X3 (X4 returns false).
  bool isX3Mode() const { return _panelSel == PanelSel::X3; }

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
  bool framebufferReady() const { return frameBuffer != nullptr; }

  // Copy the just-displayed frame (frameBufferActive) back into the write buffer.
  // displayBuffer() ends with swapBuffers(), so the write buffer would otherwise
  // hold the frame from two refreshes ago. Call this before patching a few regions
  // and re-displaying instead of fully re-rendering. No-op in single-buffer mode.
  void syncWriteBufferFromActive() const;

  // Release the framebuffer(s) — and the single-buffer async shadow — back to
  // the heap. After this call no display operations may be performed until
  // reallocBuffers() (or begin()) runs; the panel keeps showing its last
  // refreshed image. Two intended uses: transient sessions that reboot on
  // exit (e.g. a web UI), and lending ~48-100 KB to a memory-hungry phase
  // such as a chapter layout build. Safe no-op if already released.
  void releaseBuffers();

  // Reallocate after releaseBuffers(): buffers come back white (0xFF), so the
  // caller must fully redraw before the next display call. Returns false if
  // the heap cannot supply the buffers (the display is then unusable).
  bool reallocBuffers();

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  // Release only the secondary (previous-frame) buffer to free ~48-52 KB
  // temporarily — e.g. during chapter compilation when no rendering is
  // happening. Available on every dual-buffer build (not just PSRAM ones):
  // CrossPoint's C3 lends the buffer out of internal DRAM. BW display and fast
  // differential refresh continue to work: the SSD1677 driver already re-seeds
  // both BW and RED RAM when prev is null, so the differential baseline stays
  // consistent without the host copy. Grayscale AA is unavailable until restored
  // with reallocSecondaryBuffer(). No-op if already released. Returns true if freed.
  bool releaseSecondaryBuffer();

  // Reallocate the secondary buffer after releaseSecondaryBuffer(). Initialises
  // it to white (0xFF). Returns true on success; false if malloc fails.
  bool reallocSecondaryBuffer();

  // Returns true if the secondary buffer is currently allocated.
  bool hasSecondaryBuffer() const;
#endif  // !EINK_DISPLAY_SINGLE_BUFFER_MODE

  // Save the current framebuffer to a PBM file (desktop/test builds only)
  void saveFrameBufferAsPBM(const char* filename);

 private:
  void selectDriver();
  // Block until a pending async refresh completes (no-op when none is).
  // Every blocking panel operation calls this before touching the bus.
  void syncPendingAsync();
  // One framebuffer-sized heap block: PSRAM-first where available.
  static uint8_t* allocFrameBufferStorage();

  EpdPins _pins;
  EpdBus _bus;
  PanelDriver* _driver = nullptr;

  // Async refresh state: pending flag + (single-buffer mode) a lazily
  // allocated shadow of the last-displayed frame, used as the differential
  // baseline while the app redraws the live framebuffer. _shadowValid drops
  // whenever a blocking display path runs (the controller RAM then holds the
  // baseline again).
  bool _asyncPending = false;
  uint8_t* _asyncShadow = nullptr;
  bool _shadowValid = false;

  // triggerDisplay()/completeDisplay() split state. Distinct from _asyncPending:
  // a pending split is drained by the driver's displayFinish() (which runs the
  // X3 post-waveform conditioning), not by a plain bus.waitBusy().
  bool _splitPending = false;

  enum class PanelSel : uint8_t { X4, X3, M5 };
  PanelSel _panelSel = PanelSel::X4;

  // CrossPoint compatibility state (see the compatibility surface above).
  // _redRamSynced mirrors whether the X4 RED-RAM baseline is current (advisory);
  // the other two are caller preferences echoed back to CrossPoint's HAL.
  bool _redRamSynced = false;
  bool _singleBufferFastDiff = false;
  bool _fastGrayscaleLut = false;

  // Runtime display geometry (seeded from the driver at begin()).
  uint16_t displayWidth = DISPLAY_WIDTH;
  uint16_t displayHeight = DISPLAY_HEIGHT;
  uint16_t displayWidthBytes = DISPLAY_WIDTH_BYTES;
  uint32_t bufferSize = BUFFER_SIZE;

  // Frame buffer (facade-owned), heap-allocated in begin() on every build:
  // PSRAM-first on devices with it (see FREEINK_FB_PSRAM in BoardConfig.h),
  // internal DRAM otherwise. Heap-backed even without PSRAM so hosts with a
  // single tight heap (ESP32-C3) can lend the buffer out via
  // releaseBuffers()/reallocBuffers() during memory-hungry phases.
  uint8_t* frameBuffer0 = nullptr;
  uint8_t* frameBuffer = nullptr;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  uint8_t* frameBuffer1 = nullptr;
  uint8_t* frameBufferActive = nullptr;
#endif
};

}  // namespace freeink
