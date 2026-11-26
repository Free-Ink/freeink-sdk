#include <EpdiyLcd.h>
#include <EpdiyLcdDriver.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
extern "C" {
#include "e0470_epaper_waveform.h"
#include "epd_lcd.h"
#include "epdiy.h"
void check_queue(void);
}

namespace {
bool railsOk = true;
bool powered = false;
bool scanning = false;
bool gateOk = true;
bool failDraw = false;
int draws = 0, clears = 0;
int lastMode = 0;
uint8_t lastTo[32];
const EpdBoardDefinition* board = nullptr;
EpdiyHighlevelState* state = nullptr;
bool powerOn() {
  powered = railsOk;
  return powered;
}
void powerOff() { powered = scanning = false; }
bool scanActive(bool active) {
  assert(powered);
  if (!gateOk) return false;
  scanning = active;
  return true;
}
}  // namespace

namespace freeink {
const EpdiyLcdConfig& testConfig() {
  static EpdiyLcdConfig cfg{};
  cfg.power.powerOn = powerOn;
  cfg.power.powerOff = powerOff;
  cfg.power.setScanActive = scanActive;
  return cfg;
}
}  // namespace freeink
void delay(unsigned long) {}
extern "C" {
int allocation_calls = 0, fail_allocation = 0;
const EpdWaveform E0470_WAVEFORM{};
// The text-turn table lives in e0470_epaper_waveform.c, which this host test does not
// compile, so it needs a stub here too -- EpdiyLcd.cpp references it by name.
const EpdWaveform E0470_TEXTTURN_WAVEFORM{};
const EpdDisplay_t E0470_DISPLAY{16, 2, 16, 18, &E0470_WAVEFORM};
void e0470_waveform_init() {}
// EpdiyLcd.cpp prints its per-frame diagnostics through the ESP-IDF rom console, and the
// host has no such header. It declares the entry point itself under
// !defined(ESP_PLATFORM); this is the definition it needs to link.
extern "C" int esp_rom_printf(const char* format, ...) {
  (void)format;
  return 0;
}
void epd_set_board(const EpdBoardDefinition* b) { board = b; }
const EpdDisplay_t* epd_get_display() { return &E0470_DISPLAY; }
int epd_width() { return 16; }
int epd_height() { return 2; }
EpdRect epd_full_screen() { return {0, 0, 16, 2}; }
enum EpdRotation epd_get_rotation() { return EPD_ROT_LANDSCAPE; }
bool epd_init(const EpdBoardDefinition* b, const EpdDisplay_t*, enum EpdInitOptions) {
  board = b;
  return board->init(16);
}
void epd_deinit() {
  board->poweroff(nullptr);
  board->deinit();
}
void epd_poweron() { board->poweron(nullptr); }
void epd_poweroff() { board->poweroff(nullptr); }
void epd_clear() {
  assert(powered && scanning);
  ++clears;
}
esp_err_t epd_lcd_init(const LcdEpdConfig_t*, int, int) { return ESP_OK; }
void epd_lcd_set_prefill_lines(int) {}
void epd_lcd_deinit() {}
void epd_leading_skip_discard() {}
void epd_difference_column_range(EpdRect area, int* first, int* end) {
  *first = area.x;
  *end = area.x + area.width;
}
EpdRect epd_difference_image_cropped(const uint8_t* to, const uint8_t* from, EpdRect area, uint8_t* diff, bool* lines,
                                     uint8_t* columns) {
  for (int p = 0; p < 32; ++p) {
    const int shift = 4 * (p & 1);
    diff[p] = static_cast<uint8_t>(((to[p / 2] >> shift) & 15) << 4 | ((from[p / 2] >> shift) & 15));
  }
  bool changed = false;
  for (int y = 0; y < 2; ++y) {
    lines[y] = std::memcmp(to + y * 8, from + y * 8, 8) != 0;
    changed |= lines[y];
  }
  std::memset(columns, changed ? 0xFF : 0, 8);
  return changed ? area : EpdRect{0, 0, 0, 0};
}
enum EpdDrawError epd_draw_base(EpdRect, const uint8_t* diff, EpdRect, enum EpdDrawMode mode, int, const bool*,
                                const uint8_t*, const EpdWaveform*) {
  assert(powered && scanning);
  ++draws;
  for (int p = 0; p < 32; ++p) lastTo[p] = diff[p] >> 4;
  lastMode = mode & 0x3F;
  return failDraw ? EPD_DRAW_EMPTY_LINE_QUEUE : EPD_DRAW_SUCCESS;
}
}

void checkGrayPacking(freeink::EpdiyLcdDriver& driver, freeink::EpdBus& bus) {
  for (int pair = 0; pair < 64; ++pair) {
    uint8_t base[4]{}, lsb[4]{}, msb[4]{};
    uint8_t expected[32];
    for (int pixel = 0; pixel < 32; ++pixel) {
      // All 64 base/LSB/MSB pairs at every byte and row boundary.
      const unsigned bits = (pair >> (3 * (pixel & 1))) & 7;
      const uint8_t mask = 0x80 >> (pixel & 7);
      if (bits & 1) base[pixel / 8] |= mask;
      if (bits & 2) lsb[pixel / 8] |= mask;
      if (bits & 4) msb[pixel / 8] |= mask;
      expected[pixel] = (bits & 4) && !(bits & 2) ? 8 : (bits & 2) ? 3 : (bits & 1) ? 15 : 0;
    }
    driver.displayGrayscaleBase(bus, base, freeink::RefreshMode::Full, false);
    driver.copyGrayscaleLsb(bus, lsb);
    driver.copyGrayscaleMsb(bus, msb);
    driver.displayGray(bus, msb, true, nullptr, false);
    assert(std::memcmp(lastTo, expected, sizeof(expected)) == 0);
  }
}

void checkNativeGray(freeink::EpdiyLcdDriver& driver) {
  const int allocations = allocation_calls;
  uint8_t proxy[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t* frame = driver.beginGrayscale16();
  assert(frame != nullptr && driver.beginGrayscale16() == nullptr);
  const int beforeCancel = draws;
  memset(frame, 0, 16);
  driver.cancelGrayscale16();
  assert(draws == beforeCancel && !driver.commitGrayscale16(proxy));
  frame = driver.beginGrayscale16();
  for (int p = 0; p < 32; p += 2) frame[p / 2] = (p & 15) | (((p + 1) & 15) << 4);
  assert(driver.commitGrayscale16(proxy) && lastMode == MODE_GL16);
  for (int p = 0; p < 32; ++p) assert(lastTo[p] == (p & 15));
  assert(!driver.commitGrayscale16(proxy));
  frame = driver.beginGrayscale16();
  memset(frame, 0, 16);
  failDraw = true;
  assert(!driver.commitGrayscale16(proxy));
  driver.cancelGrayscale16();
  failDraw = false;
  frame = driver.beginGrayscale16();
  memset(frame, 0, 16);
  assert(driver.commitGrayscale16(proxy) && lastMode == MODE_GC16);
  assert(allocation_calls == allocations);
}

void checkHighlevel() {
  // Exercise both real highlevel copy paths without the wrapper's recovery.
  auto hl = epd_hl_init(&E0470_WAVEFORM);
  state = &hl;
  std::memset(hl.front_fb, 0, 16);
  uint8_t baseline[16];
  std::memcpy(baseline, hl.back_fb, 16);
  powered = scanning = true;
  failDraw = true;
  assert(epd_hl_update_screen(state, MODE_GL16, 20) != EPD_DRAW_SUCCESS);
  assert(std::memcmp(baseline, hl.back_fb, 16) == 0);
  assert(epd_hl_update_screen_full(state, MODE_GC16, 20) != EPD_DRAW_SUCCESS);
  assert(std::memcmp(baseline, hl.back_fb, 16) == 0);
  failDraw = false;
  assert(epd_hl_update_screen(state, MODE_GL16, 20) == EPD_DRAW_SUCCESS);
  assert(std::memcmp(hl.front_fb, hl.back_fb, 16) == 0);
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "highlevel") == 0) {
    checkHighlevel();
    return 0;
  }
  if (argc > 2 && std::strcmp(argv[1], "oom") == 0) {
    fail_allocation = std::atoi(argv[2]);
    freeink::EpdBus bus;
    freeink::EpdiyLcdDriver driver(freeink::testConfig());
    driver.begin(bus);
    assert(!freeink::epdiyLcdReady());
    assert(driver.beginGrayscale16() == nullptr);
    fail_allocation = 0;
    driver.begin(bus);
    assert(freeink::epdiyLcdReady());
    driver.deepSleep(bus);
    assert(!freeink::epdiyLcdReady());
    driver.begin(bus);
    assert(freeink::epdiyLcdReady());
    return 0;
  }
  if (argc > 1) {
    railsOk = false;
    assert(freeink::epdiyLcdBegin(freeink::testConfig(), 16, 2, true));
    const uint8_t fb[4]{};
    assert(!freeink::epdiyLcdDraw(fb, freeink::EpdiyLcdRefresh::Full, false));
    assert(clears == 0 && draws == 0 && !powered);
    railsOk = true;
    gateOk = false;
    assert(!freeink::epdiyLcdDraw(fb, freeink::EpdiyLcdRefresh::Full, false));
    assert(clears == 0 && draws == 0 && !powered);
    gateOk = true;
    assert(freeink::epdiyLcdDraw(fb, freeink::EpdiyLcdRefresh::Fast, false));
    assert(clears == 1 && draws == 1 && lastMode == MODE_GC16 && powered && !scanning);
    freeink::epdiyLcdDeepSleep();
    return 0;
  }
  freeink::EpdBus bus;
  freeink::EpdiyLcdDriver driver(freeink::testConfig());
  driver.begin(bus);
  uint8_t fb[4] = {0x55, 0xAA, 0x55, 0xAA}, gray[4]{};
  for (const auto mode : {freeink::RefreshMode::Full, freeink::RefreshMode::Fast, freeink::RefreshMode::Half}) {
    driver.displayGrayscaleBase(bus, fb, mode, false);
    driver.copyGrayscaleLsb(bus, gray);
    driver.copyGrayscaleMsb(bus, gray);
    driver.displayGray(bus, gray, true, nullptr, false);
    assert(lastMode == (mode == freeink::RefreshMode::Full ? MODE_GC16 : MODE_GL16));
    fb[0] ^= 1;
  }
  checkGrayPacking(driver, bus);
  checkNativeGray(driver);
  const int before = draws;
  assert(freeink::epdiyLcdDraw(fb, freeink::EpdiyLcdRefresh::Full, true));
  assert(freeink::epdiyLcdDraw(fb, freeink::EpdiyLcdRefresh::Full, true));
  assert(draws == before + 2);  // identical FULL must still drive the panel
  railsOk = false;
  assert(!freeink::epdiyLcdDraw(fb, freeink::EpdiyLcdRefresh::Fast, true));
  assert(draws == before + 2 && !powered);
  railsOk = true;
  failDraw = true;
  fb[0] ^= 1;
  assert(!freeink::epdiyLcdDraw(fb, freeink::EpdiyLcdRefresh::Fast, true));
  failDraw = false;
  const int failed = draws, dirtyClears = clears;
  assert(freeink::epdiyLcdDraw(fb, freeink::EpdiyLcdRefresh::Fast, true));
  assert(draws == failed + 1 && clears == dirtyClears + 1 && lastMode == MODE_GC16);

  check_queue();
}
