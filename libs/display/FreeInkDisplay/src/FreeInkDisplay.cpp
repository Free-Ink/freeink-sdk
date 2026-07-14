#include "FreeInkDisplay.h"

#include <BoardConfig.h>
#include <sdkconfig.h>

#include <cstring>
#ifndef ARDUINO
#include <fstream>
#include <vector>
#endif
#if defined(ARDUINO) && CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif
#if FREEINK_FB_PSRAM
#include <cstdlib>

#include "esp_heap_caps.h"
#endif

#include "driver/PanelDriver.h"

// Which panel drivers link is derived from the device set (-DFREEINK_DEVICE_*)
// in BoardConfig.h, included above, which defines each FREEINK_DRIVER_* to 0/1.
// A build links the drivers it can reach and selects among them at runtime
// (X3 + X4 both link in the generic C3 build; setDisplayX3() picks at runtime).

#if FREEINK_DRIVER_SSD1677
#include "driver/Ssd1677Driver.h"
#endif
#if FREEINK_DRIVER_UC8253_X3
#include "driver/Uc8253X3Driver.h"
#endif
#if FREEINK_DRIVER_ED2208
#include "driver/Ed2208M5Driver.h"
#endif
#if FREEINK_DRIVER_M5_OFFICIAL
#include "driver/M5OfficialDriver.h"
#endif
#if FREEINK_DRIVER_UC8253_MURPHY
#include "driver/Uc8253MurphyDriver.h"
#endif
#if FREEINK_DRIVER_LGFX_EPD
#include "driver/LgfxEpdDriver.h"
#endif
#if FREEINK_DRIVER_IT8951
#include "driver/It8951Driver.h"
#endif

namespace freeink {
namespace {
RefreshMode toInternal(FreeInkDisplay::RefreshMode m) {
  switch (m) {
    case FreeInkDisplay::FULL_REFRESH: return RefreshMode::Full;
    case FreeInkDisplay::HALF_REFRESH: return RefreshMode::Half;
    default: return RefreshMode::Fast;
  }
}

#if defined(ARDUINO) && CONFIG_PM_ENABLE
esp_pm_lock_handle_t displayNoLightSleepLock() {
  static esp_pm_lock_handle_t lock = nullptr;
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "freeink-display", &lock) != ESP_OK) {
      lock = nullptr;
    }
  }
  return lock;
}

class DisplayPmLock {
 public:
  DisplayPmLock() {
    _lock = displayNoLightSleepLock();
    _acquired = _lock != nullptr && esp_pm_lock_acquire(_lock) == ESP_OK;
  }

  ~DisplayPmLock() {
    if (_acquired) esp_pm_lock_release(_lock);
  }

  DisplayPmLock(const DisplayPmLock&) = delete;
  DisplayPmLock& operator=(const DisplayPmLock&) = delete;

 private:
  esp_pm_lock_handle_t _lock = nullptr;
  bool _acquired = false;
};
#else
class DisplayPmLock {
 public:
  DisplayPmLock() = default;
  DisplayPmLock(const DisplayPmLock&) = delete;
  DisplayPmLock& operator=(const DisplayPmLock&) = delete;
};
#endif
}  // namespace

FreeInkDisplay::FreeInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy)
    : _pins{sclk, mosi, cs, dc, rst, busy}, frameBuffer(nullptr)
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
      ,
      frameBufferActive(nullptr)
#endif
{
}

void FreeInkDisplay::setDisplayX3() {
  _panelSel = PanelSel::X3;
  // Swap the active profile to X3's sibling so resolution (and any board-level
  // reads, e.g. touch mapping) come from BoardProfile, like every other device.
  // Called before begin(), so the X3 driver singleton sees 792x528 at construction.
  BoardConfig::selectDevice(BoardConfig::Board::XteinkX3);
  displayWidth = X3_DISPLAY_WIDTH;
  displayHeight = X3_DISPLAY_HEIGHT;
  displayWidthBytes = X3_DISPLAY_WIDTH_BYTES;
  bufferSize = X3_BUFFER_SIZE;
}

void FreeInkDisplay::setDisplayM5PaperColor() {
  _panelSel = PanelSel::M5;
  // Landscape memory layout (panel is physically 600x400).
  displayWidth = 600;
  displayHeight = 400;
  displayWidthBytes = 600 / 8;
  bufferSize = static_cast<uint32_t>(displayWidthBytes) * displayHeight;
}

void FreeInkDisplay::selectDriver() {
  // Selection is purely _panelSel + the linked FREEINK_DRIVER_* set — no device
  // names. Multi-driver C3 builds pick X3 vs X4 via setDisplayX3(); single-driver
  // builds (M5/Murphy/de-link/LilyGo) fall through to the one linked driver below.
  switch (_panelSel) {
#if FREEINK_DRIVER_M5_OFFICIAL || FREEINK_DRIVER_ED2208
    case PanelSel::M5:
#if FREEINK_DRIVER_M5_OFFICIAL
      _driver = &m5OfficialDriver();  // M5 official M5GFX backend
#else
      _driver = &ed2208M5Driver();    // fast hand-rolled ED2208 backend
#endif
      break;
#endif
#if FREEINK_DRIVER_UC8253_X3
    case PanelSel::X3: _driver = &uc8253X3Driver(); break;
#endif
    case PanelSel::X4:
    default:
#if FREEINK_DRIVER_SSD1677
      _driver = &ssd1677Driver();
#elif FREEINK_DRIVER_UC8253_MURPHY
      _driver = &uc8253MurphyDriver();
#elif FREEINK_DRIVER_M5_OFFICIAL
      _driver = &m5OfficialDriver();
#elif FREEINK_DRIVER_ED2208
      _driver = &ed2208M5Driver();
#elif FREEINK_DRIVER_UC8253_X3
      _driver = &uc8253X3Driver();
#elif FREEINK_DRIVER_LGFX_EPD
      _driver = &lgfxEpdDriver();
#elif FREEINK_DRIVER_IT8951
      _driver = &it8951Driver();
#endif
      break;
  }
}

void FreeInkDisplay::begin() {
  DisplayPmLock pmLock;
  selectDriver();

  // External-library drivers (e.g. M5GFX) own the SPI/display hardware; only
  // bring up FreeInk's bus for native controller drivers.
  if (!_driver->usesExternalBus()) {
    // Pins come from the active board profile (set by selectDriver()/setDisplayX3),
    // not the constructor args — same source the IT8951 driver already uses, so one
    // binary drives whichever panel is runtime-selected and per-board pins (incl.
    // the EPD power-enable) are always correct. The ctor _pins are legacy and unused
    // here; a consumer no longer needs to know the panel's wiring.
    const auto& d = BoardConfig::ACTIVE.display;
    const EpdPins pins{d.sclk, d.mosi, d.cs, d.dc, d.rst, d.busy, d.powerEnable};
    _bus.begin(pins, _driver->spiHz(), _driver->busyPolarity(), _driver->spiMiso(), _driver->coCs());
  }

  const PanelGeometry geom = _driver->geometry();
  displayWidth = geom.width;
  displayHeight = geom.height;
  displayWidthBytes = geom.widthBytes;
  bufferSize = geom.bufferSize;

  // Heap-backed framebuffer(s) on every build — allocate once. MAX_BUFFER_SIZE
  // covers the largest panel in this build (one panel for a single-device
  // M5Paper bin). PSRAM-first where available, internal RAM otherwise; heap
  // rather than static storage so tight-DRAM hosts can lend the buffer out
  // via releaseBuffers()/reallocBuffers().
  if (!frameBuffer0) frameBuffer0 = allocFrameBufferStorage();
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  if (!frameBuffer1) frameBuffer1 = allocFrameBufferStorage();
#endif

  frameBuffer = frameBuffer0;
  if (frameBuffer0) memset(frameBuffer0, 0xFF, bufferSize);
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  frameBufferActive = frameBuffer1;
  if (frameBuffer1) memset(frameBuffer1, 0xFF, bufferSize);
#endif

  _driver->begin(_bus);
}

// ============================================================================
// Framebuffer composition (facade-owned; no driver involvement)
// ============================================================================

void FreeInkDisplay::clearScreen(uint8_t color) const { memset(frameBuffer, color, bufferSize); }

void FreeInkDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               bool fromProgmem) const {
  if (!frameBuffer) return;
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= displayHeight) break;
    const uint32_t destOffset = static_cast<uint32_t>(destY) * displayWidthBytes + (x / 8);
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= displayWidthBytes) break;
      frameBuffer[destOffset + col] =
          fromProgmem ? pgm_read_byte(&imageData[srcOffset + col]) : imageData[srcOffset + col];
    }
  }
}

void FreeInkDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                          bool fromProgmem) const {
  if (!frameBuffer) return;
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= displayHeight) break;
    const uint32_t destOffset = static_cast<uint32_t>(destY) * displayWidthBytes + (x / 8);
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= displayWidthBytes) break;
      const uint8_t srcByte = fromProgmem ? pgm_read_byte(&imageData[srcOffset + col]) : imageData[srcOffset + col];
      frameBuffer[destOffset + col] &= srcByte;  // only black pixels are drawn
    }
  }
}

void FreeInkDisplay::setFramebuffer(const uint8_t* bwBuffer) const { memcpy(frameBuffer, bwBuffer, bufferSize); }

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
void FreeInkDisplay::swapBuffers() {
  // When the secondary buffer has been released, frameBufferActive is null.
  // Swapping would set frameBuffer to null and corrupt every subsequent pixel
  // write. The driver already handles prev==nullptr correctly (X3 ignores it;
  // X4 re-seeds both BW and RED controller RAM after the refresh), so keeping
  // frameBuffer on the same allocation is correct here.
  if (!frameBufferActive) return;
  uint8_t* temp = frameBuffer;
  frameBuffer = frameBufferActive;
  frameBufferActive = temp;
}

FreeInkDisplay::RefreshMode FreeInkDisplay::resolveReleasedMode(RefreshMode mode) const {
  // Mirror open-x4 EInkDisplay::triggerDisplay's FAST->HALF downgrade. Only the X4
  // (SSD1677) differential keeps its previous-frame baseline in host-managed RED RAM;
  // once the secondary buffer is released there is no host copy to write into RED, so a
  // FAST refresh would diff the new frame against whatever RED still holds. Keep FAST
  // only when the caller opted into diffing against the controller's retained RED plane
  // (setSingleBufferFastDiff(true) — valid only if RED was seeded before the release, see
  // syncRedRamFromFrameBuffer). Otherwise downgrade to a self-contained HALF that writes
  // both planes and cannot ghost off a stale baseline. X3 keeps its baseline in the
  // controller (DTM1) and M5 uses a different model, so a host-side release never
  // degrades their fast path — no downgrade there.
  if (mode == FAST_REFRESH && _panelSel == PanelSel::X4 && !frameBufferActive && !_singleBufferFastDiff) {
    return HALF_REFRESH;
  }
  return mode;
}
#endif

void FreeInkDisplay::syncWriteBufferFromActive() const {
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  if (frameBuffer && frameBufferActive) memcpy(frameBuffer, frameBufferActive, bufferSize);
#endif
}

uint8_t* FreeInkDisplay::allocFrameBufferStorage() {
#if FREEINK_FB_PSRAM
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(MAX_BUFFER_SIZE, MALLOC_CAP_SPIRAM));
  if (buf) return buf;
#endif
  return static_cast<uint8_t*>(malloc(MAX_BUFFER_SIZE));
}

void FreeInkDisplay::releaseBuffers() {
  syncPendingAsync();  // a refresh in flight was fed from these buffers
  free(frameBuffer0);
  frameBuffer0 = nullptr;
  frameBuffer = nullptr;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  free(frameBuffer1);
  frameBuffer1 = nullptr;
  frameBufferActive = nullptr;
#endif
  // The lazily-allocated async baseline is framebuffer-sized too; hand it
  // back as well. It re-allocates (and re-seeds) on the next async display.
  free(_asyncShadow);
  _asyncShadow = nullptr;
  _shadowValid = false;
}

bool FreeInkDisplay::reallocBuffers() {
  if (!frameBuffer0) frameBuffer0 = allocFrameBufferStorage();
  if (!frameBuffer0) return false;
  frameBuffer = frameBuffer0;
  memset(frameBuffer0, 0xFF, bufferSize);
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  if (!frameBuffer1) frameBuffer1 = allocFrameBufferStorage();
  if (!frameBuffer1) return false;
  frameBufferActive = frameBuffer1;
  memset(frameBuffer1, 0xFF, bufferSize);
#endif
  return true;
}

uint8_t* FreeInkDisplay::lendBuildStorage(uint32_t* sizeOut) {
  if (_buildLent || frameBuffer0 == nullptr) {
    if (sizeOut != nullptr) *sizeOut = 0;
    return nullptr;
  }
  syncPendingAsync();  // a refresh in flight was reading these bytes
  _buildLent = true;
  frameBuffer = nullptr;   // rendering is unavailable while the bytes are lent
  _shadowValid = false;    // controller baseline no longer matches
  if (sizeOut != nullptr) *sizeOut = MAX_BUFFER_SIZE;  // full alloc is usable as scratch
  return frameBuffer0;     // the allocation itself is never freed, so it never moves
}

void FreeInkDisplay::returnBuildStorage() {
  if (!_buildLent) return;
  _buildLent = false;
  frameBuffer = frameBuffer0;
  if (frameBuffer0 != nullptr) memset(frameBuffer0, 0xFF, bufferSize);  // build clobbered it
  _shadowValid = false;
}

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
// Secondary-buffer release/realloc is available on every dual-buffer build, not
// just PSRAM ones: CrossPoint's C3 (no PSRAM) lends the ~48-52 KB secondary
// buffer out of internal DRAM during chapter compilation. allocFrameBufferStorage()
// is PSRAM-first with a malloc fallback, so this is correct with or without PSRAM.
bool FreeInkDisplay::releaseSecondaryBuffer() {
  if (!frameBufferActive) return false;
  if (frameBufferActive == frameBuffer0) {
    free(frameBuffer0);
    frameBuffer0 = nullptr;
  } else {
    free(frameBuffer1);
    frameBuffer1 = nullptr;
  }
  frameBufferActive = nullptr;
  return true;
}

bool FreeInkDisplay::reallocSecondaryBuffer() {
  if (frameBufferActive) return true;
  uint8_t** slot = (frameBuffer0 == nullptr) ? &frameBuffer0 : &frameBuffer1;
  *slot = allocFrameBufferStorage();
  if (!*slot) return false;
  frameBufferActive = *slot;
  // Best-effort seed with the last frame the host displayed: correct when the
  // caller reallocs before drawing the next page (frameBuffer then still holds
  // the on-screen frame), and it gives windowed updates a sane prev. But the
  // host may have scribbled or cleared the framebuffer since the last refresh
  // (blocking section builds warm image caches + clearScreen before this), so
  // the seed is UNPROVEN as a differential baseline — arm the one-shot below
  // so the next full-frame FAST diffs against the controller's retained RED
  // plane instead of pushing this copy into RED. Diffing a new page against a
  // wrong baseline leaves undriven pixels: a baked-in ghost of whatever the
  // panel showed (e.g. the indexing popup) on every section crossing. (open-x4
  // avoided this by skipping the RED write while _redRamSynced; FreeInk's
  // driver always writes RED from prev, so prev must never be trusted here.)
  if (frameBuffer) {
    memcpy(frameBufferActive, frameBuffer, bufferSize);
  } else {
    memset(frameBufferActive, 0xFF, bufferSize);
  }
  _redBaselineAuthoritative = true;
  return true;
}

const uint8_t* FreeInkDisplay::consumePrevFrameFor(RefreshMode effectiveMode) {
  const uint8_t* prev = frameBufferActive;
  if (_redBaselineAuthoritative) {
    // One-shot (see the header): after reallocSecondaryBuffer() the secondary
    // is unproven; the controller's RED RAM is the only trustworthy baseline.
    // FAST diffs against it via the prev==nullptr single-buffer path; non-fast
    // modes rewrite RED absolutely, so either way the baseline is
    // re-established and the flag can drop.
    _redBaselineAuthoritative = false;
    if (effectiveMode == FAST_REFRESH && prev != nullptr) {
      prev = nullptr;
    }
  }
  return prev;
}

bool FreeInkDisplay::hasSecondaryBuffer() const { return frameBufferActive != nullptr; }
#endif  // !EINK_DISPLAY_SINGLE_BUFFER_MODE

// ============================================================================
// Panel operations (delegated to the active driver)
// ============================================================================

void FreeInkDisplay::syncPendingAsync() {
  // Single pending state: any deferred refresh — X4 async fire or X3 split —
  // completes through the driver's displayFinish(), which waits out the
  // waveform (ISR edge wait, done-level fast path) and runs any post-waveform
  // pipeline (X3 DTM1 sync + conditioning). A plain waitBusy would skip that
  // and leave the controller mid-pipeline.
  if (!_refreshPending) return;
  DisplayPmLock pmLock;
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  _driver->displayFinish(_bus, frameBuffer);
#else
  _driver->displayFinish(_bus, frameBufferActive ? frameBufferActive : frameBuffer);
#endif
  _refreshPending = false;
}

bool FreeInkDisplay::supportsAsyncRefresh() const { return _driver != nullptr && _driver->supportsAsyncDisplay(); }

bool FreeInkDisplay::refreshBusy() {
  // Does NOT clear the pending state on completion: the driver's post-waveform
  // work (X3 DTM1 sync) must run through displayFinish(). When this returns
  // false, call waitRefreshComplete() (or any blocking display op) to drain it.
  if (!_refreshPending) return false;
  DisplayPmLock pmLock;
  return _refreshPending && _bus.isBusy();
}

void FreeInkDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  DisplayPmLock pmLock;
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  Serial.printf("[EPD] displayBuffer mode=%d off=%d\n", (int)mode, (int)turnOffScreen);
#endif
  syncPendingAsync();
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  _driver->display(_bus, frameBuffer, nullptr, toInternal(mode), turnOffScreen);
  // The blocking path resynced the controller's baseline from the live
  // framebuffer; the async shadow no longer matches what is displayed.
  _shadowValid = false;
#else
  const RefreshMode effMode = resolveReleasedMode(mode);
  _driver->display(_bus, frameBuffer, consumePrevFrameFor(effMode), toInternal(effMode), turnOffScreen);
  swapBuffers();
#endif
  // X4 re-seeds RED from the displayed frame inside display(); X3 has no RED plane.
  if (_panelSel != PanelSel::X3) _redRamSynced = true;
}

void FreeInkDisplay::displayBufferAsync(RefreshMode mode) { displayAsyncImpl(mode, /*turnOffScreen=*/false); }

void FreeInkDisplay::displayAsyncImpl(RefreshMode mode, bool turnOffScreen, bool noShadow) {
  // Blocking-fallback drivers finish the refresh inside displayAsync(); marking
  // it pending would make the next BUSY sync spin an edge-detect timeout
  // against an idle panel (X3TwoPhase: 1 s). Take the blocking path outright.
  if (!_driver->supportsAsyncDisplay()) {
    displayBuffer(mode, turnOffScreen);
    return;
  }
  DisplayPmLock pmLock;
  syncPendingAsync();
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  if (noShadow) {
    // Shadow-free contract: the caller keeps the framebuffer untouched until
    // the refresh completes and rebuilds the differential baseline itself
    // (e.g. the tiled-grayscale cleanup), so controller RAM stays the
    // baseline (prev = nullptr) and no 48 KB shadow is allocated.
    _refreshPending = _driver->displayStart(_bus, frameBuffer, nullptr, toInternal(mode), turnOffScreen);
    _shadowValid = false;
    return;
  }
  // The shadow contract lets the caller redraw the framebuffer immediately —
  // a panel whose displayFinish() re-reads the frame (X3 DTM1 sync) cannot
  // honor that; take the blocking path there. Use the noShadow entry (with its
  // frame-intact contract) or triggerDisplay() for X3 overlap.
  if (_panelSel == PanelSel::X3) {
    displayBuffer(mode, turnOffScreen);
    return;
  }
  if (_asyncShadow == nullptr) {
    _asyncShadow = static_cast<uint8_t*>(malloc(bufferSize));
    _shadowValid = false;
  }
  if (_asyncShadow == nullptr) {  // allocation failed: blocking fallback
    displayBuffer(mode, turnOffScreen);
    return;
  }
  // First async update after boot or a blocking display: the controller's RED
  // plane still holds the displayed frame (single-buffer prev = nullptr path);
  // from then on the shadow supplies the baseline on every update.
  _refreshPending =
      _driver->displayStart(_bus, frameBuffer, _shadowValid ? _asyncShadow : nullptr, toInternal(mode), turnOffScreen);
  memcpy(_asyncShadow, frameBuffer, bufferSize);
  _shadowValid = true;
#else
  (void)noShadow;  // dual-buffer: the secondary buffer is the baseline; no shadow exists
  const RefreshMode effMode = resolveReleasedMode(mode);
  // consumePrevFrameFor may return nullptr post-realloc: the driver then diffs
  // against retained RED and, being async, skips the post-refresh resync — RED
  // simply keeps that baseline until the next update rewrites it.
  _refreshPending =
      _driver->displayStart(_bus, frameBuffer, consumePrevFrameFor(effMode), toInternal(effMode), turnOffScreen);
  swapBuffers();
#endif
}

// ============================================================================
// CrossPoint EInkDisplay compatibility surface
// ============================================================================

void FreeInkDisplay::triggerDisplay(RefreshMode mode, bool turnOffScreen) {
  DisplayPmLock pmLock;
  syncPendingAsync();  // finish any prior split/async refresh before starting another
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  const bool deferred = _driver->displayStart(_bus, frameBuffer, nullptr, toInternal(mode), turnOffScreen);
  _shadowValid = false;
#else
  // Pass frameBufferActive as the previous frame, then swap so the caller draws
  // into the inactive buffer while the just-displayed frame is preserved for the
  // X3 post-waveform DTM1 sync the driver stashed a pointer to.
  const RefreshMode effMode = resolveReleasedMode(mode);
  const bool deferred = _driver->displayStart(_bus, frameBuffer, consumePrevFrameFor(effMode), toInternal(effMode),
                                              turnOffScreen);
  swapBuffers();
#endif
  _refreshPending = deferred;
  // X4 refreshes complete inline in displayStart() and re-seed RED from the
  // displayed frame; X3 has no RED plane. Keep the advisory flag truthful.
  if (_panelSel != PanelSel::X3) _redRamSynced = !deferred;
}

void FreeInkDisplay::triggerDisplayAsync(RefreshMode mode, bool turnOffScreen) {
  if (_panelSel == PanelSel::X3) {
    // X3's triggerDisplay() already returns while the waveform runs;
    // completeDisplay() remains its finish and finishDisplayAsync() a no-op.
    triggerDisplay(mode, turnOffScreen);
    return;
  }
  displayAsyncImpl(mode, turnOffScreen);
  // The waveform is reading RED; until something re-seeds it (the grayscale
  // cleanup that follows in the inline-AA flow, or the next blocking display)
  // it is not a valid post-waveform baseline. In the released-secondary case
  // the driver's async path also skips its post-refresh BW/RED resync.
  _redRamSynced = false;
}

// Compat aliases: one pending state, one finish path (see syncPendingAsync).
void FreeInkDisplay::finishDisplayAsync() { syncPendingAsync(); }

void FreeInkDisplay::completeDisplay() { syncPendingAsync(); }

void FreeInkDisplay::syncRedRamFromFrameBuffer() {
  // X3 has no host-managed previous-frame plane (its baseline lives in DTM1); the
  // advisory flag is meaningless there.
  if (_panelSel == PanelSel::X3) return;
  DisplayPmLock pmLock;
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  // Single-buffer builds: the driver reseeds RED from the framebuffer after every
  // refresh (displayImpl prev==nullptr path), so RED already holds the on-screen
  // frame — nothing to push.
  _redRamSynced = true;
#else
  // Dual-buffer: a fast refresh writes RED from `prev` only at its START, so between
  // refreshes RED holds the frame BEFORE the one now on the panel. That is fine while
  // paging (the next refresh rewrites RED), but the caller is about to release the
  // secondary buffer and switch to single-buffer fast-diff, where the first
  // prev==nullptr refresh reuses whatever RED currently holds. Push the on-screen frame
  // into RED now so that first diff has the correct baseline — this is the anti-ghost
  // seed the reader does before an indexing/build release. Only the release sites call
  // this; the normal per-page path relies on the driver's own `prev` write, so this adds
  // no per-refresh SPI cost.
  syncPendingAsync();  // never touch RED while a waveform is still reading it
  const uint8_t* onScreen = frameBufferActive ? frameBufferActive : frameBuffer;
  if (onScreen) _driver->seedPreviousFrame(_bus, onScreen);
  _redRamSynced = true;
  // The host just asserted a known baseline; the post-realloc one-shot (if
  // armed) is superseded.
  _redBaselineAuthoritative = false;
#endif
}

// Kept as a stable entry point for firmware already calling it; the logic
// lives in displayAsyncImpl's noShadow path.
void FreeInkDisplay::displayBufferAsyncNoShadow(RefreshMode mode) {
  displayAsyncImpl(mode, /*turnOffScreen=*/false, /*noShadow=*/true);
}

void FreeInkDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  DisplayPmLock pmLock;
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  Serial.printf("[EPD] displayWindow %u,%u %ux%u\n", x, y, w, h);
#endif
  syncPendingAsync();
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  _driver->displayWindow(_bus, frameBuffer, nullptr, x, y, w, h, turnOffScreen);
  _shadowValid = false;
#else
  _driver->displayWindow(_bus, frameBuffer, frameBufferActive, x, y, w, h, turnOffScreen);
#endif
}

void FreeInkDisplay::displayGrayBuffer(bool turnOffScreen, const unsigned char* lut, bool factoryMode) {
  DisplayPmLock pmLock;
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  Serial.printf("[EPD] displayGrayBuffer\n");
#endif
  syncPendingAsync();
  _shadowValid = false;
  _redRamSynced = false;  // grayscale leaves RED holding a gray plane, not the BW baseline
  _driver->displayGray(_bus, frameBuffer, turnOffScreen, lut, factoryMode);
}

void FreeInkDisplay::refreshDisplay(RefreshMode mode, bool turnOffScreen) { displayBuffer(mode, turnOffScreen); }

void FreeInkDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  DisplayPmLock pmLock;
  syncPendingAsync();  // RAM writes must not race a deferred refresh
  _driver->copyGrayscaleLsb(_bus, lsbBuffer);
  _driver->copyGrayscaleMsb(_bus, msbBuffer);
}

void FreeInkDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  DisplayPmLock pmLock;
  syncPendingAsync();
  _shadowValid = false;
  _driver->displayGrayscaleBase(_bus, frameBuffer, toInternal(fallback), turnOffScreen);
}

void FreeInkDisplay::preconditionGrayscale() {
  DisplayPmLock pmLock;
  syncPendingAsync();
  _driver->preconditionGrayscale(_bus, 0, 0, getDisplayWidth(), getDisplayHeight());
}

void FreeInkDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  DisplayPmLock pmLock;
  syncPendingAsync();
  _driver->preconditionGrayscale(_bus, x, y, w, h);
}

void FreeInkDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  DisplayPmLock pmLock;
  syncPendingAsync();
  _driver->copyGrayscaleLsb(_bus, lsbBuffer);
}

void FreeInkDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  DisplayPmLock pmLock;
  syncPendingAsync();
  _driver->copyGrayscaleMsb(_bus, msbBuffer);
}

void FreeInkDisplay::writeGrayscalePlaneStrip(GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                              uint16_t numRows) {
  DisplayPmLock pmLock;
  syncPendingAsync();  // no-op in the reader flow (it waits first); guards misuse
  _driver->writeGrayscalePlaneStrip(_bus, plane == GRAY_PLANE_LSB ? freeink::GrayPlane::Lsb : freeink::GrayPlane::Msb,
                                    rows, yStart, numRows);
}

bool FreeInkDisplay::supportsStripGrayscale() const { return _driver && _driver->supportsStripGrayscale(); }

void FreeInkDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  DisplayPmLock pmLock;
  syncPendingAsync();
  _driver->cleanupGrayscaleBuffers(_bus, bwBuffer);
  // Restore frameBuffer so subsequent BW draws paint onto a valid BW baseline
  // rather than the stale LSB/MSB grayscale plane data that was there before.
  if (frameBuffer && bwBuffer && frameBuffer != bwBuffer)
    memcpy(frameBuffer, bwBuffer, bufferSize);
}
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
void FreeInkDisplay::cleanupGrayscaleWithPreviousBuffer() {
  DisplayPmLock pmLock;
  const uint8_t* baseline = frameBufferActive ? frameBufferActive : frameBuffer;
  _driver->cleanupGrayscaleBuffers(_bus, baseline);
  if (frameBuffer && baseline && frameBuffer != baseline)
    memcpy(frameBuffer, baseline, bufferSize);
}
#endif

void FreeInkDisplay::requestResync(uint8_t settlePasses) {
  if (_driver) _driver->requestResync(settlePasses);
}

void FreeInkDisplay::skipInitialResync() {
  if (_driver) _driver->skipInitialResync();
}

void FreeInkDisplay::requestCompleteWaveformNextRefresh() {
  if (_driver) _driver->requestCompleteWaveformNextRefresh();
}

void FreeInkDisplay::setFastRefreshCutoffMs(uint16_t ms) {
  if (_driver) _driver->setFastRefreshCutoffMs(ms);
}

uint16_t FreeInkDisplay::fastRefreshCutoffMs() const {
  return _driver ? _driver->fastRefreshCutoffMs() : 0;
}

void FreeInkDisplay::grayscaleRevert() {
  DisplayPmLock pmLock;
  if (_driver) _driver->grayscaleRevert(_bus, frameBuffer);
}

void FreeInkDisplay::setCustomLUT(bool enabled, const unsigned char* lutData) {
  DisplayPmLock pmLock;
  if (_driver) _driver->setCustomLut(_bus, enabled, lutData);
}

void FreeInkDisplay::deepSleep() {
  DisplayPmLock pmLock;
  syncPendingAsync();
  if (_driver) _driver->deepSleep(_bus);
}

// ============================================================================
// Desktop/test helper
// ============================================================================

void FreeInkDisplay::saveFrameBufferAsPBM(const char* filename) {
#ifndef ARDUINO
  const uint8_t* buffer = getFrameBuffer();
  std::ofstream file(filename, std::ios::binary);
  if (!file) return;

  // Rotate 90 degrees counterclockwise: 800x480 landscape -> 480x800 portrait.
  const int W = DISPLAY_WIDTH;
  const int H = DISPLAY_HEIGHT;
  const int WB = W / 8;

  file << "P4\n" << H << " " << W << "\n";
  std::vector<uint8_t> rotated((H / 8) * W, 0);
  for (int outY = 0; outY < W; outY++) {
    for (int outX = 0; outX < H; outX++) {
      const int inX = outY;
      const int inY = H - 1 - outX;
      const int inByte = inY * WB + (inX / 8);
      const int inBit = 7 - (inX % 8);
      const bool isWhite = (buffer[inByte] >> inBit) & 1;
      if (!isWhite) {
        const int outByte = outY * (H / 8) + (outX / 8);
        const int outBit = 7 - (outX % 8);
        rotated[outByte] |= (1 << outBit);
      }
    }
  }
  file.write(reinterpret_cast<const char*>(rotated.data()), rotated.size());
#else
  (void)filename;
#endif
}

}  // namespace freeink
