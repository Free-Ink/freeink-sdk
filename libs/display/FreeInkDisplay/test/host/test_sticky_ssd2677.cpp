// Seeed Sticky with the SSD2677 panel: CMD 0x70 probe, stock init table, OTP
// waveform latch per mode, full refresh old == new, rows bottom-to-top, fast
// refresh keeping the rails on.
#include <SPI.h>  // stub: counts shared-bus restarts
#include <esp_heap_caps.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/driver/Ssd2677Driver.h"
#include "src/lut/Ssd2677Luts.h"
#undef private

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static const Bytes* lastData(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command) return &i->bytes;
  return nullptr;
}

static int count(const EpdBus& bus, uint8_t command) {
  int n = 0;
  for (const auto& w : bus.writes) n += w.command == command;
  return n;
}

int main(int argc, char** argv) {
  const bool originalPanel = argc > 1 && !strcmp(argv[1], "ssd1677");
  // An SSD2677 answers CMD 0x70 with 0x07 first; the probe restarts the shared
  // SPI bus on its existing pins (incl. the SD card's MISO) afterwards.
  static const uint8_t rev[3] = {0x07, 0x01, 0x01};
  if (!originalPanel) memcpy(fakePanelRev, rev, 3);
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  assert(display.getControllerName() == nullptr && fakePanelCommand == -1 && fakeSpiRestarts == 0);
  display.selectDriver();
  const char* expected = originalPanel ? "SSD1677" : "SSD2677";
  assert(!strcmp(display.getControllerName(), expected));
  assert(fakePanelCommand == 0x70 && fakeSpiRestarts == 1);
  // The board profile still says SSD1677; diagnostics must follow the selected driver.
  assert(BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::SSD1677);
  assert(!strcmp(display.getControllerName(), expected) && fakeSpiRestarts == 1);
  if (originalPanel) {
    std::puts("Sticky SSD1677: detected controller identity passed");
    return 0;
  }
  assert(stickyHasSsd2677());

  auto& s = ssd2677Driver();
  assert(s.busyPolarity() == BusyPolarity::ActiveLowReady);

  EpdBus bus;
  fakeHeapCapsFail = argc > 1;
  s.begin(bus);
  // Wait after reset, panel setting and the final E9 command, exactly once
  // each. A second wait after E9 looks for a new BUSY pulse for 100 ms even
  // though no command was sent. AA reinitializes twice per steady-state turn.
  assert(bus.busyWaitWriteCounts == std::vector<size_t>({0, 1, 10}));
  if (fakeHeapCapsFail) {
    assert(static_cast<Ssd2677Driver&>(s)._old == nullptr);
    // Both black and the following white frame must use the full waveform
    // when there is no previous plane for a differential update.
    for (uint8_t color : {uint8_t{0x00}, uint8_t{0xFF}}) {
      Bytes page(48000, color);
      bus.clear();
      s.display(bus, page.data(), nullptr, RefreshMode::Fast, false);
      assert(*lastData(bus, 0xE6) == Bytes({0xEE}));
      assert(*lastData(bus, 0x10) == Bytes(96000, color));
      assert(count(bus, 0x02) == 1 && "fallback powers off after full refresh");
    }
    std::puts("Sticky SSD2677: previous-plane allocation failure uses full refresh");
    return 0;
  }
  assert(*lastData(bus, 0x00) == Bytes({0x2F, 0x0E}));
  assert(*lastData(bus, 0x61) == Bytes({0x03, 0x20, 0x02, 0xA8}));
  assert(*lastData(bus, 0x30) == Bytes({0x08}) && *lastData(bus, 0xE9) == Bytes({0x01}));
  assert(count(bus, 0x83) == 0 && count(bus, 0x20) == 0 && "no window, no LUT upload");

  // First paint is forced FULL: OTP full waveform latched, old == new, power off.
  Bytes page(48000, 0xFF);
  page[0] = 0x0F;  // row 0: left 4 px black, right 4 px white
  bus.clear();
  s.display(bus, page.data(), nullptr, RefreshMode::Fast, false);
  assert(*lastData(bus, 0xE0) == Bytes({0x12}) && *lastData(bus, 0xE6) == Bytes({0xEE}) && count(bus, 0xA5) == 1);
  assert(count(bus, 0x20) == 0);
  const Bytes* data = lastData(bus, 0x10);
  assert(data && data->size() == 96000);
  const size_t row0 = static_cast<size_t>(479) * 100 * 2;  // rows go bottom-to-top
  assert((*data)[row0] == 0x00 && (*data)[row0 + 1] == 0xFF && "full: (0,0)x4 then (1,1)x4");
  assert((*data)[0] == 0xFF && "the bottom row (white) is sent first");
  assert(*lastData(bus, 0x12) == Bytes({0x00}) && count(bus, 0x02) == 1 && "full powers off");

  // Fast: the waveform changes, so stock re-runs init before latching it; data
  // diffs against the retained old plane; the rails stay on.
  Bytes white(48000, 0xFF);
  bus.clear();
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(count(bus, 0x00) == 1 && "waveform change re-inits the panel");
  assert(*lastData(bus, 0xE6) == Bytes({0x19}));
  data = lastData(bus, 0x10);
  assert((*data)[row0] == 0x55 && (*data)[row0 + 1] == 0xFF && "fast: (old 0, new 1)x4 then (1,1)x4");
  assert(count(bus, 0x02) == 0 && "fast keeps the rails on");

  // Another fast frame: same waveform, no re-init.
  bus.clear();
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(count(bus, 0x00) == 0 && *lastData(bus, 0xE6) == Bytes({0x19}));

  // Absolute gray (stock gray4): no B/W base; the OTP gray waveform gets the
  // level index per pixel (00 black, 01 dark, 10 light, 11 white), then power
  // off; the next B/W frame is a full refresh.
  assert(s.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(!s.grayscaleCapabilities(GrayscaleMode::Direct).supported());
  Bytes lsb(48000, 0xFF), msb(48000, 0xFF);
  lsb[0] = 0x5F;  // px 0-3: black 0, dark 1, light 0, white 1
  msb[0] = 0x3F;  // px 0-3: black 0, dark 0, light 1, white 1
  bus.clear();
  s.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, RefreshMode::Fast, false);
  assert(bus.writes.empty() && "no base refresh");
  s.copyGrayscaleLsb(bus, lsb.data());
  s.copyGrayscaleMsb(bus, msb.data());
  assert(*lastData(bus, 0xE6) == Bytes({0xFA}));
  data = lastData(bus, 0x10);
  assert(data && (*data)[row0] == 0x1B && (*data)[row0 + 1] == 0xFF && "00 01 10 11 then white");
  s.displayGray(bus, white.data(), false, nullptr, true);
  assert(count(bus, 0x12) == 1 && bus.writes.back().command == 0x02 && "gray refresh powers off");
  bus.clear();
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(*lastData(bus, 0xE6) == Bytes({0xEE}) && "B/W after a gray image is a full refresh");

  // AA (overlay): the base re-inks black pixels using the fast OTP waveform, then the masks
  // (MSB high, LSB low, like the Picco) are nudged gray with the uploaded AA
  // waveform; the next frame re-runs init before latching its OTP waveform.
  assert(s.grayscaleCapabilities(GrayscaleMode::Overlay).supported());
  Bytes lsbMask(48000, 0x00), msbMask(48000, 0x00);
  lsbMask[0] = 0xF0;  // px 0-3 dark
  msbMask[0] = 0xCC;  // px 0,1,4,5 gray
  // Top four pixels: B->B, B->W, W->B, W->W. Check the reversed panel row
  // independently from the blank row at the start of the transfer.
  page[0] = 0x3F;
  s.display(bus, page.data(), nullptr, RefreshMode::Fast, false);
  page[0] = 0x5F;
  bus.clear();
  s.beginGrayscale(bus, page.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  assert(count(bus, 0x12) == 1 && *lastData(bus, 0xE6) == Bytes({0x19}) && "overlay base is a fast B/W refresh");
  data = lastData(bus, 0x10);
  assert(data && (*data)[row0] == 0x9B && (*data)[row0 + 1] == 0xFF && (*data)[0] == 0xFF);
  assert(count(bus, 0x02) == 0 && "fast overlay base keeps the rails on");
  bus.clear();
  s.copyGrayscaleLsb(bus, lsbMask.data());
  s.copyGrayscaleMsb(bus, msbMask.data());
  assert(bus.busyWaitWriteCounts == std::vector<size_t>({0, 1, 10}));
  assert(count(bus, 0x00) == 1 && count(bus, 0xE6) == 0 && *lastData(bus, 0xE0) == Bytes({0x10}) &&
         "masks go out after a fresh init, no OTP latch");
  data = lastData(bus, 0x10);
  assert(data && (*data)[row0] == 0xF5 && (*data)[row0 + 1] == 0xA0 && "px (msb,lsb) = 11 11 01 01 | 10 10 00 00");
  s.displayGray(bus, white.data(), false, nullptr, false);
  const Bytes* lut = lastData(bus, 0x20);
  assert(lut && *lut == Bytes(lut_picco_se0400_gray, lut_picco_se0400_gray + 535));
  assert(count(bus, 0x12) == 1);
  bus.clear();
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(count(bus, 0x00) == 1 && *lastData(bus, 0xE6) == Bytes({0x19}) && "next frame re-inits after the AA LUT");
  data = lastData(bus, 0x10);
  assert(data && (*data)[row0] == 0x77 && "ordinary fast erase uses the actual retained base");
  // The next base still waits after reset/initialization, OTP latch and power
  // on, and still performs its refresh. Only the duplicate init wait is gone.
  assert(bus.busyWaitWriteCounts == std::vector<size_t>({0, 1, 10, 13, 15}));
  assert(bus.waits == 6);

  // Deep sleep: the rails are on after a fast frame, so they go off first.
  bus.clear();
  s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  bus.clear();
  s.deepSleep(bus);
  assert(bus.writes.size() >= 2 && bus.writes.front().command == 0x02 && *lastData(bus, 0x07) == Bytes({0xA5}));
  std::printf("Sticky SSD2677: probe, init, OTP latch, packing, row order, power, gray4 and AA passed\n");
  return 0;
}
