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

  // True when displayStart() defers (returns true) rather than completing
  // inline. Lets the facade skip async scaffolding (shadow setup) on blocking
  // drivers without a trial call, and lets hosts size overlap buffers up
  // front. Must agree with what displayStart() actually returns.
  virtual bool supportsAsyncDisplay() const { return false; }

  // Two-call refresh split (CrossPoint EInkDisplay::triggerDisplay/completeDisplay).
  // For the shadowed async path the facade passes its own baseline copy as
  // `prev`, so the live fb may be redrawn immediately; otherwise `fb` must
  // stay intact until displayFinish() returns:
  // controllers whose post-waveform pipeline re-reads the host frame (UC8253 X3
  // syncs DTM1 and runs conditioning passes after BUSY) need it. The contract is
  // the caller does non-SPI CPU work in the gap and issues no other bus op until
  // displayFinish().
  //
  // displayStart() loads RAM, fires the waveform, and either:
  //   - returns true  -> a waveform is in flight; displayFinish() must run to
  //                      wait it out and do post-waveform work, or
  //   - returns false -> the refresh completed synchronously (nothing deferred);
  //                      displayFinish() is then a no-op.
  // The default is the fully-blocking display() (returns false), so a driver
  // gains the split only by overriding both. SSD1677 (X4) keeps the default:
  // its refresh is short and its post-waveform RED re-seed already lives inside
  // display(), matching CrossPoint's "X4 completes inline" behavior.
  virtual bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
    display(bus, fb, prev, mode, turnOff);
    return false;
  }
  // `fb` is the just-displayed frame, re-supplied fresh by the facade at finish
  // time (not stashed at start): callers may release/realloc the buffer holding
  // it between the two calls, so the driver must not cache the pointer.
  virtual void displayFinish(EpdBus& bus, const uint8_t* fb) { (void)bus; (void)fb; }

  // Re-seed the controller's host-managed previous-frame plane (SSD1677 RED RAM)
  // with `buf`, WITHOUT triggering a refresh. A dual-buffer fast refresh only
  // writes RED from `prev` at its start, so between refreshes RED holds the frame
  // BEFORE the one on the panel. That is fine while paging (the next refresh
  // rewrites RED) but wrong at the moment the host releases its secondary buffer
  // for single-buffer fast-diff: the first prev==nullptr refresh reuses whatever
  // RED holds. Callers seed the on-screen frame here just before releasing so that
  // first differential diff has a correct baseline. Default no-op: controllers with
  // no host-managed previous-frame plane (X3 DTM1, M5) keep their own baseline.
  virtual void seedPreviousFrame(EpdBus& bus, const uint8_t* buf) { (void)bus; (void)buf; }

  // --- grayscale (dual-plane LSB/MSB) ---
  virtual bool supportsStripGrayscale() const { return false; }
  // True when displayGrayscaleBase() DEFERS the base activation so the gray
  // planes join it in a single waveform (Paper Mono). Hosts should then route the
  // grayscale base through displayGrayscaleBase() instead of display(): a
  // separate B/W refresh first makes the gray pass re-drive the whole text
  // body through the custom LUT's kick phases (a visible flash).
  virtual bool combinesGrayscaleBase() const { return false; }
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
  // Host-retained selector planes can be copied and encoded while the previous
  // B/W waveform is BUSY. Drivers returning true must not touch SPI in either
  // writeGrayscalePlaneStrip() or prepareGrayscaleTarget().
  virtual bool supportsBusyGrayscaleStaging() const { return false; }
  virtual void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                        uint16_t numRows) {
    (void)bus; (void)plane; (void)rows; (void)yStart; (void)numRows;
  }
  virtual void prepareGrayscaleTarget(const uint8_t* bw) { (void)bw; }
  virtual void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) {
    (void)lut;
    (void)factoryMode;
    display(bus, fb, nullptr, RefreshMode::Fast, turnOff);
  }
  // Diagnostic four-gray comparison. The full frame is first rendered with
  // the controller's flashing OTP waveform; `custom*` is then rebuilt with the
  // driver's non-flashing grayscale path. Default drivers keep the OTP frame.
  virtual void displayGrayCalibration(EpdBus& bus, const uint8_t* fb, uint16_t customX, uint16_t customY,
                                      uint16_t customW, uint16_t customH) {
    (void)customX;
    (void)customY;
    (void)customW;
    (void)customH;
    displayGray(bus, fb, false, nullptr, true);
  }
  virtual void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) { (void)bus; (void)bw; }

  // --- optional, controller-specific hooks (no-op by default) ---
  virtual void requestResync(uint8_t settlePasses) { (void)settlePasses; }
  virtual void skipInitialResync() {}
  // Content-polarity hint: true while the facade is rendering inverted (dark
  // background) frames. Differential drivers idle unchanged pixels, so on a
  // dark background the residue of every white->black transition parks in the
  // background and accumulates — worst on panels whose corrective pass is
  // non-flashing. Drivers may use this to widen their drive set (re-blacken
  // the unchanged background each update) or bias their deghost direction.
  virtual void setBackgroundHint(bool darkBackground) { (void)darkBackground; }
  // Capture the cancellation generation at the start of a logical UI render.
  // This must happen before CPU-side composition: input arriving while an old
  // frame is being composed must still cancel its optional post-refresh work.
  virtual void beginDisplayWork() {}
  // Cancel optional work which follows the primary B/W refresh (grayscale
  // refinement or ghost cleanup). Drivers should only stop between panel
  // waveforms: an already-triggered waveform must still run to completion.
  virtual void abortPostRefresh() {}
  virtual bool postRefreshAborted() const { return false; }
  // True when a frame actually reached the panel since beginDisplayWork().
  // Drivers that paint synchronously inside display() always commit, so the
  // default is true and their callers are unaffected. Paper Mono batches a
  // three-level target in host RAM across several calls and legitimately
  // discards it when a page turn is superseded; a caller whose periodic
  // ghost-cleanup cadence or explicit refresh request is consumed on submit
  // rather than on commit would silently lose it. Query after the whole
  // display sequence, not between its halves.
  virtual bool displayCommitted() const { return true; }
  // Run deferred panel maintenance after the visible frame has been committed.
  // The default is deliberately empty; only panels with a non-flashing cleanup
  // waveform need it.
  virtual void runMaintenance(EpdBus& bus) { (void)bus; }
  virtual bool hasPendingMaintenance() const { return false; }
  // Called by the single controller-work consumer only after both foreground
  // and maintenance queues are empty. Panels which keep their analog/clock
  // domains alive across adjacent waveforms can shut them down here.
  virtual void controllerIdle(EpdBus& bus) { (void)bus; }
  virtual void requestCompleteWaveformNextRefresh() {}
  // Standing-image policy (ED2208): when enabled, RefreshMode::Full always
  // runs the panel's complete OTP waveform (~15 s, DC-balanced, true white,
  // full color) instead of an interrupted full-panel pass. Lets a consumer
  // whose Full refreshes are all standing images (e.g. a clock/dashboard) get
  // a clean render on every one without threading the one-shot
  // requestCompleteWaveformNextRefresh() through each call site. Default off:
  // consumers that page with Full (readers) keep the fast behavior.
  virtual void setFullRefreshCompletesWaveform(bool enabled) { (void)enabled; }
  // Accent color planes (ED2208, Spectra-6): each `plane` is a 1-bit buffer
  // with the same logical geometry and layout as the framebuffer; a SET bit
  // recolors that pixel's ink (a 0/black framebuffer bit) to that slot's
  // `colorCode` on complete-waveform refreshes. Interrupted refreshes ignore
  // the planes (color pigments never settle in a cut-off waveform), so
  // accents appear only on standing images. Up to 4 slots; the lowest-
  // numbered slot with a set bit wins where planes overlap. nullptr clears a
  // slot. The caller owns the buffers and must keep them valid across
  // refreshes.
  virtual void setAccentPlaneSlot(uint8_t slot, const uint8_t* plane, uint8_t colorCode) {
    (void)slot;
    (void)plane;
    (void)colorCode;
  }
  // Interrupted-refresh cutoff tuning (ED2208: where the gate scan freezes).
  virtual void setFastRefreshCutoffMs(uint16_t ms) { (void)ms; }
  virtual uint16_t fastRefreshCutoffMs() const { return 0; }
  // Hold the periodic anti-ghost full refresh through a continuous interaction
  // (e.g. a live slider drag): while set, fast refreshes are never promoted to a
  // full, so the drag stays flash-free. Clear it and force one full afterward to
  // scrub any accumulated ghost. No-op on drivers without a periodic-full cadence.
  virtual void setHoldPeriodicFull(bool hold) { (void)hold; }
  virtual void grayscaleRevert(EpdBus& bus, const uint8_t* fb) { (void)bus; (void)fb; }
  virtual void setCustomLut(EpdBus& bus, bool enabled, const unsigned char* data) { (void)bus; (void)enabled; (void)data; }
};

}  // namespace freeink
