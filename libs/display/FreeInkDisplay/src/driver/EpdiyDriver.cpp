#include "EpdiyDriver.h"

#include <BoardConfig.h>

#if FREEINK_DRIVER_EPDIY
#include <MetalioPlusEpdBoard.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

// Use the firmware serial logger when available; IDF logs may use another console.
#if __has_include(<Logging.h>)
#include <Logging.h>
#else
#define LOG_INF ESP_LOGI
#define LOG_ERR ESP_LOGE
#endif

#include <cstring>

extern "C" {
#include <epd_display.h>
#include <epd_highlevel.h>
#include <epdiy.h>
#include <waveforms/my_waveform.h>
}
#endif

namespace freeink {

#if FREEINK_DRIVER_EPDIY
namespace {

constexpr const char* TAG = "epdiy_drv";

EpdiyHighlevelState g_hl;
bool g_started = false;
bool g_resyncPending = false;
// The boot clear leaves a faint ghost of a dark standing image (the sleep
// cover), and a differential first frame never drives the pixels that stay
// white. So the first frame after begin() drives every pixel (as Half) whatever
// the wake reason; skipInitialResync() does not cancel it.
bool g_firstFrameAllPixels = false;
// False after a failed draw: the glass then differs from the front buffer, so
// an unchanged next frame must still be drawn.
bool g_glassMatchesFront = false;
// A grayscale base staged by displayGrayscaleBase() and not drawn yet, and the
// refresh mode it asked for.
bool g_basePending = false;
RefreshMode g_baseMode = RefreshMode::Fast;

// A Fast update changing more than this share of the screen (a new screen rather
// than a menu move) runs GC16 on the changed pixels instead: DU leaves a visible
// ghost of the previous screen on this panel.
constexpr uint32_t kLargeChangeDivisor = 4;

// One 1-bpp byte (MSB = leftmost pixel, set = white) as four 4-bpp bytes.
// EPDiy stores the even pixel in the low nibble.
uint32_t g_expand[256];

void buildExpandTable() {
  for (int b = 0; b < 256; ++b) {
    uint32_t out = 0;
    for (int pair = 0; pair < 4; ++pair) {
      const uint8_t even = (b & (0x80 >> (2 * pair))) ? 0x0F : 0x00;
      const uint8_t odd = (b & (0x40 >> (2 * pair))) ? 0xF0 : 0x00;
      out |= static_cast<uint32_t>(even | odd) << (8 * pair);
    }
    g_expand[b] = out;
  }
}

// Returns how many 8-pixel groups differ from the frame EPDiy last drew. The
// front buffer still holds that frame until it is overwritten here.
uint32_t expandFrame(const uint8_t* fb) {
  const PanelGeometry geom = epdiyDriver().geometry();
  auto* front = reinterpret_cast<uint32_t*>(epd_hl_get_framebuffer(&g_hl));
  const uint32_t total = geom.bufferSize;
  uint32_t changed = 0;
  for (uint32_t i = 0; i < total; ++i) {
    const uint32_t next = g_expand[fb[i]];
    changed += front[i] != next;
    front[i] = next;
  }
  return changed;
}

// Overlay grey levels on EPDiy's 0 (black) .. 15 (white) scale, and the two
// 1-bpp selector planes the facade sends (PSRAM, allocated on first use).
// A flatbed scan of a 16-level ramp drawn with MY_WAVEFORM's GC16 put level 3
// at about 22% of the way from black to the paper, 5 at 40%, 7 at 62% and 10 at
// 86%. With 0x5/0xA the light grey nearly matched the paper and text looked
// aliased. 3 and 5 are slightly bold; 4 and 7 would split the range evenly, but
// the difference needs a magnifier.
constexpr uint8_t kGrayDark = 0x3;
constexpr uint8_t kGrayLight = 0x5;
uint8_t* g_lsb = nullptr;
uint8_t* g_msb = nullptr;

uint8_t* planeBuffer(GrayPlane plane) {
  uint8_t*& buf = plane == GrayPlane::Lsb ? g_lsb : g_msb;
  if (!buf) {
    buf = static_cast<uint8_t*>(heap_caps_calloc(epdiyDriver().geometry().bufferSize, 1, MALLOC_CAP_SPIRAM));
    if (!buf) LOG_ERR(TAG, "grey plane allocation failed");
  }
  return buf;
}

// Paint the planes' grey pixels over the B/W base the last display() left in
// the front buffer. The facade's framebuffer holds the last plane by now, not
// the page (see LgfxEpdDriver's overlayCanvasGray), so it is not used.
// Returns false when the planes select no pixel.
bool overlayGray() {
  if (!g_lsb || !g_msb) return false;
  bool any = false;
  uint8_t* front = epd_hl_get_framebuffer(&g_hl);
  const uint32_t total = epdiyDriver().geometry().bufferSize;
  for (uint32_t i = 0; i < total; ++i) {
    const uint8_t l = g_lsb[i];
    const uint8_t m = g_msb[i];
    if ((l | m) == 0) continue;
    any = true;
    uint8_t* px = front + i * 4;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint8_t mask = 0x80 >> bit;
      if (((l | m) & mask) == 0) continue;
      const uint8_t level = (m & mask) && !(l & mask) ? kGrayLight : kGrayDark;
      uint8_t& pair = px[bit / 2];
      pair = (bit & 1) ? static_cast<uint8_t>((pair & 0x0F) | (level << 4)) : static_cast<uint8_t>((pair & 0xF0) | level);
    }
  }
  return any;
}

// How a frame reaches the glass.
enum class Draw : uint8_t {
  Du,           // changed pixels to black/white: fast, ghosts
  Gc16Changed,  // changed pixels with the grey waveform; unchanged ones keep their residue
  Gc16All,      // every pixel, unchanged ones included: cleans residue, flashes
  Clean,        // black/white clear cycles, then every pixel from white (vendor's deep clean)
};

void drawFront(Draw how) {
  const int64_t startUs = esp_timer_get_time();
  epd_poweron();
  if (!metalioplus::panelPowerGood()) {
    LOG_ERR(TAG, "panel power failed; frame not drawn");
    epd_poweroff();
    g_glassMatchesFront = false;
    return;
  }
  const int temperature = static_cast<int>(epd_ambient_temperature());
  // Changed-pixel GC16 (page turns, screen changes, AA pages) uses the trimmed
  // 36-phase GC16 from my_waveform_fast() instead of the full 48 phases. The
  // all-pixel and clean draws keep the full waveform. NULL = trimmed table not
  // allocated, full waveform kept.
  const EpdWaveform* fast = my_waveform_fast();
  epd_hl_waveform(&g_hl, how == Draw::Gc16Changed && fast ? fast : &MY_WAVEFORM);
  const auto run = [&]() -> enum EpdDrawError {
    switch (how) {
      case Draw::Du:
        return epd_hl_update_screen(&g_hl, MODE_DU, temperature);
      case Draw::Gc16Changed:
        return epd_hl_update_screen(&g_hl, MODE_GC16, temperature);
      case Draw::Gc16All:
        return epd_hl_update_screen_full(&g_hl, MODE_GC16, temperature);
      case Draw::Clean: {
        const enum EpdDrawError cleared = epd_clear_checked();
        return cleared == EPD_DRAW_SUCCESS ? epd_hl_update_screen_from_white(&g_hl, MODE_GC16, temperature) : cleared;
      }
    }
    return EPD_DRAW_SUCCESS;
  };
  enum EpdDrawError err = run();
  if (err & EPD_DRAW_EMPTY_LINE_QUEUE) {
    // A line-stream fault aborts the waveform partway, leaving pixels half
    // driven. EPDiy only records a frame as drawn when it succeeds, so running
    // it again drives the same pixels from their recorded level to the target.
    LOG_ERR(TAG, "line stream fault; redrawing frame");
    err = run();
    // Still failing: drive every pixel on the next frame.
    if (err != EPD_DRAW_SUCCESS) g_resyncPending = true;
  }
  // Leaving the TPS65185 rails up between frames holds VCOM on the glass, which
  // EPDiy advises against, so they are dropped after every frame.
  epd_poweroff();
  static const char* const kDrawNames[] = {"DU", "GC16 changed", "GC16 all", "clean"};
  LOG_INF(TAG, "%s frame: %lld ms waveform=%s skipped=%d result=0x%x",
          kDrawNames[static_cast<uint8_t>(how)],
          static_cast<long long>((esp_timer_get_time() - startUs) / 1000),
          how == Draw::Gc16Changed && fast ? "trimmed" : "full", epd_last_leading_skip(),
          static_cast<unsigned>(err));
  g_glassMatchesFront = err == EPD_DRAW_SUCCESS;
  if (err != EPD_DRAW_SUCCESS) LOG_ERR(TAG, "EPDiy update failed: 0x%x", static_cast<unsigned>(err));
}

}  // namespace

PanelGeometry EpdiyDriver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight;
  const uint16_t wb = w / 8;
  return {w, h, wb, static_cast<uint32_t>(wb) * h};
}

void EpdiyDriver::begin(EpdBus& bus) {
  (void)bus;
  if (g_started) return;
  g_started = true;
  buildExpandTable();
  // EPD_LUT_64K selects the S31 pixel-pair lookup; the 1K lookup underruns the
  // LCD stream on this panel (see libs/display/Epdiy/NOTICE.md).
  epd_init(&metalioplus::epdBoard(), &E0470A14_AF, EPD_LUT_64K);
  epd_set_vcom(metalioplus::DEFAULT_VCOM_MV);
  epd_set_rotation(EPD_ROT_LANDSCAPE);
  my_waveform_init();
  g_hl = epd_hl_init(&MY_WAVEFORM);
  // Keep leading hold phases while checking their effect on transition residue.
  epd_set_leading_skip(false);
  // EPDiy assumes the glass starts white. Make it so, as the vendor firmware
  // does at boot, so the first differential update has a true baseline. Not
  // epd_fullclear(): it asserts on a draw error, which would reboot every boot.
  epd_hl_set_all_white(&g_hl);
  epd_poweron();
  if (metalioplus::panelPowerGood()) {
    const enum EpdDrawError err = epd_clear_checked();
    if (err != EPD_DRAW_SUCCESS) LOG_ERR(TAG, "boot clear failed: 0x%x", static_cast<unsigned>(err));
  } else {
    LOG_ERR(TAG, "panel power failed; boot clear skipped");
  }
  epd_poweroff();
  g_firstFrameAllPixels = true;
}

void EpdiyDriver::requestResync(uint8_t settlePasses) {
  // One full GC16 pass already drives every pixel to its target here.
  (void)settlePasses;
  g_resyncPending = true;
}

void EpdiyDriver::skipInitialResync() { g_resyncPending = false; }

void EpdiyDriver::deepSleep(EpdBus& bus) {
  (void)bus;
  epd_poweroff();
}

void EpdiyDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;
  (void)turnOff;  // the rails are always dropped below
  if (!g_started || !fb) return;
  const uint32_t changed = expandFrame(fb);
  g_basePending = false;  // a plain frame replaces any staged grayscale base
  // CrossPoint's Half is its periodic and screen-change cleanup, so it drives
  // every pixel (the vendor firmware draws its GC16 that way too); driving only
  // the changed ones left traces of earlier DU frames in areas that stayed white.
  Draw how = mode == RefreshMode::Full ? Draw::Clean : mode == RefreshMode::Half ? Draw::Gc16All : Draw::Du;
  if (g_resyncPending || g_firstFrameAllPixels) {
    how = Draw::Gc16All;
    g_resyncPending = false;
    g_firstFrameAllPixels = false;
  } else if (how == Draw::Du && changed > geometry().bufferSize / kLargeChangeDivisor) {
    how = Draw::Gc16Changed;
  } else if (how == Draw::Du && changed == 0 && g_glassMatchesFront) {
    return;  // nothing to drive; skip the TPS65185 power-up
  }

  drawFront(how);
}

void EpdiyDriver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  uint8_t* dst = planeBuffer(GrayPlane::Lsb);
  if (dst && lsb) memcpy(dst, lsb, geometry().bufferSize);
}

void EpdiyDriver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  uint8_t* dst = planeBuffer(GrayPlane::Msb);
  if (dst && msb) memcpy(dst, msb, geometry().bufferSize);
}

void EpdiyDriver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                           uint16_t numRows) {
  (void)bus;
  uint8_t* dst = planeBuffer(plane);
  if (!dst || !rows) return;
  const PanelGeometry geom = geometry();
  memcpy(dst + static_cast<uint32_t>(yStart) * geom.widthBytes, rows, static_cast<size_t>(numRows) * geom.widthBytes);
}

void EpdiyDriver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                              bool factoryMode) {
  (void)bus;
  (void)fb;
  (void)turnOff;
  (void)lut;
  (void)factoryMode;
  if (!g_started) return;
  const bool grey = overlayGray();
  if (!grey && !g_basePending) return;
  // One waveform for the staged base and the grey pixels: GC16 on the pixels
  // that differ from the glass, or the base's own full refresh.
  Draw how = Draw::Gc16Changed;
  if (g_basePending) {
    if (g_baseMode == RefreshMode::Full) how = Draw::Clean;
    if (g_baseMode == RefreshMode::Half) how = Draw::Gc16All;
    if (g_resyncPending || g_firstFrameAllPixels) {
      how = Draw::Gc16All;
      g_resyncPending = false;
      g_firstFrameAllPixels = false;
    }
    g_basePending = false;
  }
  drawFront(how);
}

void EpdiyDriver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  (void)bus;
  (void)turnOff;
  if (!g_started || !fb) return;
  expandFrame(fb);
  g_basePending = true;
  g_baseMode = fallback;
  // Until displayGray() draws it, the glass no longer matches the front buffer.
  g_glassMatchesFront = false;
}

PanelDriver& epdiyDriver() {
  static EpdiyDriver instance;
  return instance;
}

#endif  // FREEINK_DRIVER_EPDIY

}  // namespace freeink
