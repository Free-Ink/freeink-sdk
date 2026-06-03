#include "M5OfficialDriver.h"

#include <BoardConfig.h>  // for FREEINK_DRIVER_M5_OFFICIAL

#if FREEINK_DRIVER_M5_OFFICIAL
#include <M5Unified.h>  // pulls M5GFX; added to lib_deps only on the official env
#endif

namespace freeink {
namespace {
// Match the native ED2208 driver's landscape layout so the app draws the same
// 600x400 framebuffer regardless of which backend is selected.
constexpr uint16_t M5_W = 600;
constexpr uint16_t M5_H = 400;
constexpr uint16_t M5_WB = M5_W / 8;
constexpr uint32_t M5_BUFFER = static_cast<uint32_t>(M5_WB) * M5_H;
}  // namespace

PanelGeometry M5OfficialDriver::geometry() const { return {M5_W, M5_H, M5_WB, M5_BUFFER}; }

void M5OfficialDriver::begin(EpdBus& bus) {
  (void)bus;
#if FREEINK_DRIVER_M5_OFFICIAL
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);  // landscape => 600x400, matching the framebuffer
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
#endif
}

void M5OfficialDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;
#if FREEINK_DRIVER_M5_OFFICIAL
  // Full refresh -> quality waveform; otherwise the fastest waveform M5GFX offers.
  M5.Display.setEpdMode(mode == RefreshMode::Full ? epd_mode_t::epd_quality : epd_mode_t::epd_fastest);

  // Blit the 1bpp framebuffer (bit set = white, per FreeInk convention). A
  // per-pixel push is simple and correct; once validated on hardware this can be
  // swapped for a single pushImage/pushGrayscaleImage for speed.
  M5.Display.startWrite();
  for (uint16_t y = 0; y < M5_H; ++y) {
    const uint8_t* row = fb + static_cast<uint32_t>(y) * M5_WB;
    for (uint16_t x = 0; x < M5_W; ++x) {
      const bool white = (row[x >> 3] >> (7 - (x & 7))) & 0x01;
      M5.Display.drawPixel(x, y, white ? TFT_WHITE : TFT_BLACK);
    }
  }
  M5.Display.endWrite();
  M5.Display.display();  // commit the e-paper refresh

  if (turnOff) {
    M5.Display.sleep();
  }
#else
  (void)fb;
  (void)mode;
  (void)turnOff;
#endif
}

void M5OfficialDriver::deepSleep(EpdBus& bus) {
  (void)bus;
#if FREEINK_DRIVER_M5_OFFICIAL
  M5.Display.sleep();
#endif
}

PanelDriver& m5OfficialDriver() {
  static M5OfficialDriver instance;
  return instance;
}

}  // namespace freeink
