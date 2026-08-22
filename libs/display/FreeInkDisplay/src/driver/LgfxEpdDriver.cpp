#include "LgfxEpdDriver.h"

#include <BoardConfig.h>

#include <cstring>

#if FREEINK_DRIVER_LGFX_EPD
#include <M5GFX.h>  // pulls LovyanGFX; added to lib_deps only on the LilyGo env
#include <esp_heap_caps.h>
#include <lgfx/v1/platforms/esp32/Bus_EPD.h>
#include <lgfx/v1/platforms/esp32/Panel_EPD.hpp>
#endif

namespace freeink {

#if FREEINK_DRIVER_LGFX_EPD
namespace {

// Set from the active config in begin(), read by the bus subclass below. The
// driver is a singleton (one panel), so a file-scope pointer is fine and mirrors
// how M5GFX/LovyanGFX use global device objects.
const LgfxEpdPowerHooks* g_hooks = nullptr;

// Bus subclass that defers the board's power topology to injected hooks. Matches
// the two override points LovyanGFX exposes: init() (pin setup) and
// powerControl() (rail up/down), guarding the _pwr_on state itself. A board
// whose rails are plain GPIOs (pinOe/pinPwr/pinSpv, e.g. M5Stack PaperS3) leaves
// the corresponding hook null and gets Bus_EPD's stock power sequence instead;
// a board with external power silicon (LilyGo's TPS65185 + PCA9535) hooks it.
class FreeInkBusEPD : public lgfx::Bus_EPD {
 public:
  bool init() override {
    if (g_hooks && g_hooks->prepare && !g_hooks->prepare()) return false;
    return lgfx::Bus_EPD::init();
  }

  bool powerControl(const bool powerOn) override {
    if (_pwr_on == powerOn) return true;
    const bool hooked = g_hooks && (powerOn ? g_hooks->powerOn != nullptr : g_hooks->powerOff != nullptr);
    if (!hooked) return lgfx::Bus_EPD::powerControl(powerOn);
    wait();
    if (powerOn) {
      if (!g_hooks->powerOn()) return false;
      _pwr_on = true;
      return true;
    }
    g_hooks->powerOff();
    _pwr_on = false;
    return true;
  }
};

class FreeInkLgfxEpd : public lgfx::LGFX_Device {
 public:
  void setup(const LgfxEpdConfig& c, uint16_t w, uint16_t h) {
    auto bc = _bus.config();
    bc.bus_speed = c.busHz;
    for (int i = 0; i < 8; ++i) bc.pin_data[i] = c.dataPins[i];
    bc.pin_pwr = c.pinPwr;
    bc.pin_sph = c.pinSph;
    bc.pin_spv = c.pinSpv;
    bc.pin_oe = c.pinOe;
    bc.pin_le = c.pinLe;
    bc.pin_cl = c.pinCl;
    bc.pin_ckv = c.pinCkv;
    bc.bus_width = 8;
    _bus.config(bc);

    _panel.setBus(&_bus);

    auto dc = _panel.config_detail();
    dc.line_padding = c.linePadding;
    if (c.lutQuality && c.lutQualityStep) {
      dc.lut_quality = c.lutQuality;
      dc.lut_quality_step = c.lutQualityStep;
    }
    if (c.lutText && c.lutTextStep) {
      dc.lut_text = c.lutText;
      dc.lut_text_step = c.lutTextStep;
    }
    if (c.lutFast && c.lutFastStep) {
      dc.lut_fast = c.lutFast;
      dc.lut_fast_step = c.lutFastStep;
    }
    if (c.lutFastest && c.lutFastestStep) {
      dc.lut_fastest = c.lutFastest;
      dc.lut_fastest_step = c.lutFastestStep;
    }
    _panel.config_detail(dc);

    auto pc = _panel.config();
    pc.memory_width = pc.panel_width = w;
    pc.memory_height = pc.panel_height = h;
    pc.offset_rotation = 0;
    pc.offset_x = 0;
    pc.offset_y = 0;
    pc.bus_shared = false;
    _panel.config(pc);

    setPanel(&_panel);
  }

 private:
  FreeInkBusEPD _bus;
  lgfx::Panel_EPD _panel;
};

FreeInkLgfxEpd g_dev;

lgfx::epd_mode::epd_mode_t epdModeFor(RefreshMode m) {
  switch (m) {
    case RefreshMode::Full: return lgfx::epd_mode::epd_text;
    case RefreshMode::Half: return lgfx::epd_mode::epd_text;
    default: return lgfx::epd_mode::epd_fast;
  }
}

// 8-bit gray canvas (PSRAM) the panel pushes from, plus the two 1-bpp planes the
// facade streams for grayscale. Allocated once in begin().
lgfx::LGFX_Sprite* g_canvas = nullptr;
uint8_t* g_lsb = nullptr;
uint8_t* g_msb = nullptr;
uint16_t g_w = 0, g_h = 0, g_wb = 0;

// 0x00 and 0xFF survive Panel_EPD's quantiser at both rails whatever the Bayer
// cell (it clamps), so the two rails need no board input. The greys do -- see
// LgfxEpdConfig::grayDark.
constexpr uint8_t kGrayBlack = 0x00, kGrayWhite = 0xFF;
uint8_t g_grayDark = 0, g_grayLight = 0;

void allocCanvas(uint16_t w, uint16_t h) {
  g_w = w;
  g_h = h;
  g_wb = w / 8;
  if (!g_canvas) {
    g_canvas = new lgfx::LGFX_Sprite(&g_dev);
    g_canvas->setPsram(true);
    g_canvas->setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    g_canvas->createSprite(w, h);
  }
  const size_t planeBytes = static_cast<size_t>(g_wb) * h;
  if (!g_lsb) g_lsb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
  if (!g_msb) g_msb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
}

// Expand a 1-bpp B/W frame (bit set = white) into the 8-bit gray canvas.
void fillCanvasBW(const uint8_t* fb) {
  if (!g_canvas) return;
  auto* dst = static_cast<uint8_t*>(g_canvas->getBuffer());
  if (!dst) return;
  for (uint16_t y = 0; y < g_h; ++y) {
    const uint8_t* src = fb + static_cast<uint32_t>(y) * g_wb;
    uint8_t* drow = dst + static_cast<uint32_t>(y) * g_w;
    for (uint16_t bx = 0; bx < g_wb; ++bx) {
      const uint8_t b = src[bx];
      for (uint8_t bit = 0; bit < 8; ++bit) drow[bx * 8 + bit] = (b & (0x80 >> bit)) ? kGrayWhite : kGrayBlack;
    }
  }
}

// Overlay the buffered LSB/MSB planes onto the B/W canvas the base push left
// behind, darkening only the pixels a plane actually selects.
//
// This used to take the base frame as an argument and rebuild every pixel from
// it. That looked reasonable but could not work: displayGray() is handed
// FreeInkDisplay::frameBuffer, and the host's plane dance (clear to 0x00, render
// text-only, copy the plane out, call displayGray) leaves the LAST PLANE there,
// not the page. Ssd1677Driver::displayGray() opens with `(void)fb` -- its planes
// are already in controller RAM and the panel retains the B/W image -- so nothing
// ever noticed that the buffer held a plane. Here it painted the whole background
// black and left only the anti-aliased marks standing.
//
// The canvas already holds the B/W frame from the base push and is not cleared by
// pushSprite(), so it IS the base. Reading it instead of a caller-supplied pointer
// removes the ambiguity rather than relying on the caller to resolve it.
void overlayCanvasGray() {
  if (!g_canvas || !g_lsb || !g_msb) return;
  auto* dst = static_cast<uint8_t*>(g_canvas->getBuffer());
  if (!dst) return;
  for (uint16_t y = 0; y < g_h; ++y) {
    const uint8_t* lrow = g_lsb + static_cast<uint32_t>(y) * g_wb;
    const uint8_t* mrow = g_msb + static_cast<uint32_t>(y) * g_wb;
    uint8_t* drow = dst + static_cast<uint32_t>(y) * g_w;
    for (uint16_t bx = 0; bx < g_wb; ++bx) {
      const uint8_t l = lrow[bx], m = mrow[bx];
      if ((l | m) == 0) continue;  // no selector bits in this byte — leave the B/W run alone
      for (uint8_t bit = 0; bit < 8; ++bit) {
        const uint8_t mask = 0x80 >> bit;
        const bool lb = (l & mask) != 0, mb = (m & mask) != 0;
        if (!lb && !mb) continue;
        drow[bx * 8 + bit] = (mb && !lb) ? g_grayLight : g_grayDark;
      }
    }
  }
}

// The epd_mode of the last base push. Panel_EPD's per-pixel diff keys on the
// epd_mode LUT offset, so the grayscale overlay must be pushed with the SAME mode
// the base used or every pixel is re-driven (a full-screen flash). displayGray()
// used to hardcode epd_fast while display() maps HALF/FULL to epd_text, so any
// page refreshed with those modes flashed when its AA pass ran.
lgfx::epd_mode::epd_mode_t g_lastBaseEpdMode = lgfx::epd_mode::epd_fast;

// Wait out a refresh this driver just queued.
//
// waitDisplay() alone can return before the refresh has begun: Panel_EPD's
// display() raises _display_busy, yields (vTaskDelay(1)), and only then posts
// the job. The yield lets the panel task reach the top of its loop, where it
// assigns _display_busy = remain unconditionally -- false on an idle panel --
// clearing the flag the caller just raised, then blocking on a queue the job
// has not reached. Between xQueueSend() returning and the task waking, the flag
// reads false for a refresh that has not started, so a caller that trusts it
// walks straight into the panel task's diff copy and tears it. Torn step state
// is how a pixel ends up with a step index that never terminates, `remain`
// never clears, and the next waitDisplay() blocks forever -- the reader frozen
// with input still alive.
//
// Yielding first lets the panel task ingest the job and re-raise the flag; the
// wait after it then means what it says. (1.5.16 shipped this, 1.5.17 reverted
// it on a ghosting suspicion; the ghosting survived the revert, which clears
// this guard of that charge.)
void settleDisplay() {
  vTaskDelay(pdMS_TO_TICKS(2));
  g_dev.waitDisplay();
}

void pushCanvas(lgfx::epd_mode::epd_mode_t epdMode) {
  if (!g_canvas) return;
  g_dev.waitDisplay();
  g_dev.setEpdMode(epdMode);
  g_canvas->pushSprite(0, 0);  // commits to the panel; Panel_EPD runs the refresh
  settleDisplay();
}

// Push the canvas keeping its grey levels, then refresh through the differential
// bank.
//
// Panel_EPD reads the epd_mode twice, at two different moments, and they do not
// have to agree. _draw_pixels() reads it while the sprite is being copied into
// the panel's 4bpp buffer, and in epd_fast/epd_fastest it Bayer-dithers every
// pixel to one of the two rails there and then -- that is what turned the AA
// greys into hard black speckle along glyph edges. task_update() reads it again
// when the refresh is queued, and only the fast modes skip lut_eraser, the
// preliminary pass that drives everything toward mid grey and shows as a flash.
//
// Splitting auto-display lets each read see the mode it should: quality while the
// pixels land (16 levels, no dither), fast when the refresh goes out (no eraser,
// and the same LUT bank the B/W base used, so Panel_EPD's per-pixel diff still
// skips everything that did not change).
void pushCanvasGraded(lgfx::epd_mode::epd_mode_t refreshMode) {
  if (!g_canvas) return;
  g_dev.waitDisplay();
  g_dev.setEpdMode(lgfx::epd_mode::epd_quality);
  g_dev.setAutoDisplay(false);
  g_canvas->pushSprite(0, 0);  // writes the panel buffer, queues no refresh
  g_dev.setAutoDisplay(true);
  g_dev.setEpdMode(refreshMode);
  g_dev.display();  // covers the rect pushSprite accumulated
  settleDisplay();
}

}  // namespace
#endif  // FREEINK_DRIVER_LGFX_EPD

LgfxEpdDriver::LgfxEpdDriver(const LgfxEpdConfig& cfg) : _cfg(cfg) {}

PanelGeometry LgfxEpdDriver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight;
  const uint16_t wb = w / 8;
  return {w, h, wb, static_cast<uint32_t>(wb) * h};
}

void LgfxEpdDriver::begin(EpdBus& bus) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  g_hooks = &_cfg.power;
  g_dev.setup(_cfg, BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
  g_dev.init();
  g_dev.setRotation(_cfg.rotation);
  g_dev.setEpdMode(lgfx::epd_mode::epd_fast);
  g_grayDark = _cfg.grayDark;
  g_grayLight = _cfg.grayLight;
  allocCanvas(BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
#endif
}

void LgfxEpdDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;
#if FREEINK_DRIVER_LGFX_EPD
  fillCanvasBW(fb);  // expand the 1-bpp frame into the gray canvas
  g_lastBaseEpdMode = epdModeFor(mode);
  pushCanvas(g_lastBaseEpdMode);
  if (turnOff) g_dev.sleep();
#else
  (void)fb;
  (void)mode;
  (void)turnOff;
#endif
}

// One render, one push: the whole page -- text and its anti-aliasing greys --
// reaches the panel as a single waveform.
//
// The two-push flow this replaces (B/W base, then a grey overlay push) existed
// to normalize fringe pixels to black before a from-black grey nudge, because a
// destination-indexed LUT cannot see where a pixel came from. The fast bank's
// grey columns are now self-normalizing (saturate at the white rail, then walk
// down to the level), so the base pass has nothing left to do and the page has
// no intermediate state to show: it arrives finished, or it has not arrived.
//
// The charge story rides on the same property. Under the old flow every fringe
// pixel swung black-to-grey through two pushes on every page turn, with a net
// drive imbalance each time; under one push, Panel_EPD's diff drives a pixel
// only when its target changes, and every grey drive begins with a saturating
// rail visit that erases accumulated bias.
void LgfxEpdDriver::displayGrayFrame(EpdBus& bus, const uint8_t* fb, RefreshMode mode, bool turnOff) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  if (!fb) return;
  g_dev.waitDisplay();  // never write the canvas while a refresh may be in flight
  fillCanvasBW(fb);
  overlayCanvasGray();
  // HALF/FULL map to the GC16-style clean bank, whose columns land every level
  // exactly, so the periodic scrub page carries its greys too. FAST takes the
  // differential bank. Either way the write itself must be graded -- a fast-mode
  // write Bayer-dithers the greys to the rails before any LUT is consulted.
  g_lastBaseEpdMode = epdModeFor(mode);
  pushCanvasGraded(g_lastBaseEpdMode);
  if (turnOff) g_dev.sleep();
#else
  (void)fb;
  (void)mode;
  (void)turnOff;
#endif
}

void LgfxEpdDriver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  if (g_lsb && lsb) memcpy(g_lsb, lsb, static_cast<size_t>(g_wb) * g_h);
#else
  (void)lsb;
#endif
}

void LgfxEpdDriver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  if (g_msb && msb) memcpy(g_msb, msb, static_cast<size_t>(g_wb) * g_h);
#else
  (void)msb;
#endif
}

void LgfxEpdDriver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                             uint16_t numRows) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  uint8_t* dstPlane = (plane == GrayPlane::Lsb) ? g_lsb : g_msb;
  if (!dstPlane || !rows) return;
  const uint32_t offset = static_cast<uint32_t>(yStart) * g_wb;
  memcpy(dstPlane + offset, rows, static_cast<size_t>(numRows) * g_wb);
#else
  (void)plane;
  (void)rows;
  (void)yStart;
  (void)numRows;
#endif
}

void LgfxEpdDriver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)bus;
  (void)lut;
  (void)factoryMode;
#if FREEINK_DRIVER_LGFX_EPD
  (void)fb;             // the canvas from the base push IS the base; see overlayCanvasGray()
  overlayCanvasGray();  // darken only the pixels the planes select
  // Refresh under the mode the base push used. Panel_EPD's per-pixel diff keys
  // on the epd_mode LUT offset, so switching modes here re-drives every pixel --
  // a full-screen flash on any page the host refreshed with HALF or FULL.
  //
  // The pixel write is a separate question from the refresh, and on a board
  // whose fast bank carries grey columns it must not go out under a fast mode:
  // _draw_pixels() Bayer-dithers to the two rails there, which is what turned
  // the greys into black speckle. pushCanvasGraded() writes under a graded mode
  // and refreshes under this one.
  if (_cfg.grayNudgeInFastBank) {
    pushCanvasGraded(g_lastBaseEpdMode);
  } else {
    pushCanvas(g_lastBaseEpdMode);
  }
  if (turnOff) g_dev.sleep();
#else
  (void)fb;
  (void)turnOff;
#endif
}

void LgfxEpdDriver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  if (!bw) return;
  fillCanvasBW(bw);
#else
  (void)bw;
#endif
}

void LgfxEpdDriver::deepSleep(EpdBus& bus) {
  (void)bus;
#if FREEINK_DRIVER_LGFX_EPD
  g_dev.sleep();
#endif
}

// Per-board config injection. This driver has NO universal default — the bus pins
// and power hooks are entirely board-specific — so a LilyGo-class board defines
// `const LgfxEpdConfig& yourConfig();` in namespace freeink and builds with
// -DFREEINK_LGFX_EPD_CONFIG=yourConfig. The SDK's board-support libraries provide
// the default configs for FREEINK_DEVICE_LILYGO (BoardT5S3) and
// FREEINK_DEVICE_PAPERS3 (BoardPaperS3) builds.
#if FREEINK_DEVICE_LILYGO
const LgfxEpdConfig& lilygoT5S3LgfxConfig();
PanelDriver& lgfxEpdDriver() {
  static LgfxEpdDriver instance(lilygoT5S3LgfxConfig());
  return instance;
}
#elif FREEINK_DEVICE_PAPERS3 && !defined(FREEINK_LGFX_EPD_CONFIG)
const LgfxEpdConfig& m5PaperS3LgfxConfig();
PanelDriver& lgfxEpdDriver() {
  static LgfxEpdDriver instance(m5PaperS3LgfxConfig());
  return instance;
}
#elif defined(FREEINK_LGFX_EPD_CONFIG)
const LgfxEpdConfig& FREEINK_LGFX_EPD_CONFIG();
PanelDriver& lgfxEpdDriver() {
  static LgfxEpdDriver instance(FREEINK_LGFX_EPD_CONFIG());
  return instance;
}
#elif FREEINK_DRIVER_LGFX_EPD
#error "FREEINK_DRIVER_LGFX_EPD requires a board config: define `const LgfxEpdConfig& yourConfig();` in namespace freeink and build with -DFREEINK_LGFX_EPD_CONFIG=yourConfig"
#else
// Driver not selected in this build: provide a stub so the accessor still links if
// referenced. Never called (the facade only selects it under FREEINK_DRIVER_LGFX_EPD).
PanelDriver& lgfxEpdDriver() {
  static const LgfxEpdConfig kNone = {};
  static LgfxEpdDriver instance(kNone);
  return instance;
}
#endif

}  // namespace freeink
