#include "EpdiyDriver.h"

#if FREEINK_DRIVER_EPDIY

#include <esp_heap_caps.h>

#include <cstring>

#ifndef FREEINK_EPDIY_CONFIG
#error \
    "FREEINK_DRIVER_EPDIY requires a board config: define `const EpdiyConfig& yourConfig();` in namespace freeink and build with -DFREEINK_EPDIY_CONFIG=yourConfig"
#endif

namespace freeink {

const EpdiyConfig& FREEINK_EPDIY_CONFIG();

namespace {

// EPDiy 4 bpp: two pixels per byte, even pixel in the low nibble, 0 = black,
// 15 = white.
constexpr uint8_t kBlack = 0x0, kDark = 0x5, kLight = 0xA, kWhite = 0xF;

// One 1 bpp byte (MSB = leftmost pixel, set = white) -> four 4 bpp bytes.
uint32_t gExpand[256];
bool gExpandReady = false;

void buildExpand() {
  if (gExpandReady) return;
  for (int b = 0; b < 256; ++b) {
    uint32_t out = 0;
    for (int pair = 0; pair < 4; ++pair) {
      const uint8_t even = (b & (0x80 >> (pair * 2))) ? kWhite : kBlack;
      const uint8_t odd = (b & (0x40 >> (pair * 2))) ? kWhite : kBlack;
      out |= static_cast<uint32_t>(even | (odd << 4)) << (pair * 8);
    }
    gExpand[b] = out;
  }
  gExpandReady = true;
}

enum EpdDrawMode modeFor(RefreshMode mode) {
  switch (mode) {
    case RefreshMode::Full:
    case RefreshMode::Half:
      return MODE_GC16;
    case RefreshMode::Fast:
      break;
  }
  return MODE_DU;
}

}  // namespace

EpdiyDriver::EpdiyDriver(const EpdiyConfig& cfg) : _cfg(cfg) {}

PanelGeometry EpdiyDriver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight;
  const uint16_t wb = w / 8;
  return {w, h, wb, static_cast<uint32_t>(wb) * h};
}

int EpdiyDriver::temperature() const {
  const float t = epd_ambient_temperature();
  return (t > 0.0f && t < 50.0f) ? static_cast<int>(t) : _cfg.temperatureC;
}

void EpdiyDriver::begin(EpdBus& bus) {
  (void)bus;
  if (_ready) return;
  buildExpand();
  epd_init(_cfg.board, _cfg.display, _cfg.initOptions);
  if (_cfg.vcomMv > 0) epd_set_vcom(static_cast<uint16_t>(_cfg.vcomMv));
  // Native scan order; the host renders in the panel's own 1216x684 frame.
  epd_set_rotation(EPD_ROT_LANDSCAPE);
  if (_hl.front_fb == nullptr) {
    _hl = epd_hl_init(_cfg.waveform ? _cfg.waveform : _cfg.display->default_waveform);
  }
  const uint32_t planeBytes = geometry().bufferSize;
  if (_lsb == nullptr) _lsb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
  if (_msb == nullptr) _msb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
  if (_lsb) memset(_lsb, 0, planeBytes);
  if (_msb) memset(_msb, 0, planeBytes);
  _ready = _hl.front_fb != nullptr;
}

void EpdiyDriver::deepSleep(EpdBus& bus) {
  (void)bus;
  if (!_ready) return;
  epd_poweroff();
  epd_deinit();
  _ready = false;
}

// Rails up only for the waveform; the panel is powered off between refreshes.
// A differential update only drives pixels EPDiy's previous frame says changed,
// so the first draw clears the glass and draws from white, and full refreshes
// drive every pixel (the vendor firmware's sequence).
bool EpdiyDriver::update(enum EpdDrawMode mode, bool full) {
  epd_poweron();
  enum EpdDrawError err;
  if (!_panelKnown) {
    err = epd_clear_checked();
    if (err == EPD_DRAW_SUCCESS) err = epd_hl_update_screen_from_white(&_hl, MODE_GC16, temperature());
  } else if (full) {
    err = epd_hl_update_screen_full(&_hl, mode, temperature());
  } else {
    err = epd_hl_update_screen(&_hl, mode, temperature());
  }
  epd_poweroff();
  // An interrupted waveform leaves the optical state unknown: clear again next time.
  _panelKnown = err == EPD_DRAW_SUCCESS;
  return _panelKnown;
}

void EpdiyDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;  // EPDiy's highlevel state keeps the previous frame
  (void)turnOff;  // always powered off after the waveform
  if (!_ready || fb == nullptr) return;
  const PanelGeometry g = geometry();
  auto* dst = reinterpret_cast<uint32_t*>(epd_hl_get_framebuffer(&_hl));
  for (uint32_t i = 0; i < g.bufferSize; ++i) dst[i] = gExpand[fb[i]];
  _lastBaseMode = modeFor(mode);
  update(_lastBaseMode, mode != RefreshMode::Fast);
}

void EpdiyDriver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (_lsb && lsb) memcpy(_lsb, lsb, geometry().bufferSize);
}

void EpdiyDriver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  if (_msb && msb) memcpy(_msb, msb, geometry().bufferSize);
}

void EpdiyDriver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                           uint16_t numRows) {
  (void)bus;
  uint8_t* dst = plane == GrayPlane::Lsb ? _lsb : _msb;
  if (!dst || !rows) return;
  const PanelGeometry g = geometry();
  if (yStart >= g.height) return;
  if (numRows > g.height - yStart) numRows = g.height - yStart;
  memcpy(dst + static_cast<uint32_t>(yStart) * g.widthBytes, rows, static_cast<size_t>(numRows) * g.widthBytes);
}

void EpdiyDriver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                              bool factoryMode) {
  (void)bus;
  (void)fb;  // the base page is already in EPDiy's framebuffer from display()
  (void)turnOff;
  (void)lut;
  (void)factoryMode;
  if (!_ready || !_lsb || !_msb) return;
  const PanelGeometry g = geometry();
  uint8_t* dst = epd_hl_get_framebuffer(&_hl);
  for (uint32_t i = 0; i < g.bufferSize; ++i) {
    const uint8_t l = _lsb[i], m = _msb[i];
    if ((l | m) == 0) continue;
    for (int bit = 0; bit < 8; ++bit) {
      const uint8_t mask = 0x80 >> bit;
      if (!((l | m) & mask)) continue;
      const uint8_t level = (l & mask) ? kDark : kLight;
      const uint32_t px = i * 8 + bit;
      uint8_t& cell = dst[px / 2];
      cell = (px & 1) ? static_cast<uint8_t>((cell & 0x0F) | (level << 4)) : static_cast<uint8_t>((cell & 0xF0) | level);
    }
  }
  // A clean (GC16) base keeps its mode; otherwise GL16 moves only the gray pixels.
  update(_lastBaseMode == MODE_GC16 ? MODE_GC16 : MODE_GL16, false);
}

PanelDriver& epdiyDriver() {
  static EpdiyDriver driver(FREEINK_EPDIY_CONFIG());
  return driver;
}

}  // namespace freeink

#endif  // FREEINK_DRIVER_EPDIY
