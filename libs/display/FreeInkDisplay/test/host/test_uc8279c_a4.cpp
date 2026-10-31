#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

#include "driver/Uc8279cA4Driver.h"

int main() {
  using namespace freeink;
  constexpr size_t bytes = 768 / 8 * 552;
  std::vector<uint8_t> bw(bytes, 0xff), gray(bytes, 0xaa);
  EpdBus bus;
  auto& driver = uc8279cA4Driver();
  driver.begin(bus);

  // A real reader AA turn: base -> gray masks -> restore base -> different page.
  // The target differs from both the old base and the gray selector planes.
  for (size_t i = 0; i < bytes; ++i) bw[i] = static_cast<uint8_t>(i * 37);
  driver.display(bus, bw.data(), nullptr, RefreshMode::Full, false);
  driver.copyGrayscaleLsb(bus, gray.data());
  driver.copyGrayscaleMsb(bus, gray.data());
  driver.displayGray(bus, bw.data(), false, nullptr, false);
  driver.cleanupGrayscaleBuffers(bus, bw.data());
  for (auto& b : bw) b = static_cast<uint8_t>(~b);
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.interval == 0xd7 && bus.lutBytes == 49);
  // X4's post-AA FAST path drives every target pixel, including old text to white.
  for (size_t i = 0; i < bytes; ++i) {
    const auto physical = (551 - i / 96) * 96 + i % 96;
    assert(bus.newAtRefresh[physical] == bw[i]);
    assert(bus.oldAtRefresh[physical] == static_cast<uint8_t>(~bw[i]) &&
           "post-AA FAST leaves white target pixels without a clearing transition");
  }
  assert(bus.oldPlane == bus.newPlane && "displayed B/W frame was not retained as the next baseline");
  // The next ordinary FAST paint must diff against the displayed frame.
  const auto previous = bus.newAtRefresh;
  for (auto& b : bw) b = static_cast<uint8_t>(b ^ 0x5a);
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, true);
  assert(bus.oldAtRefresh == previous && "ordinary FAST diff lost its previous displayed frame");
  assert(bus.oldPlane == bus.newPlane);
  driver.begin(bus);

  // Unknown glass after boot still gets a full first paint.
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.interval == 0x97 && bus.lutBytes == 49);

  // Menu moves do not acquire an independent periodic-full cadence.
  for (int move = 0; move < 20; ++move) {
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    assert(bus.interval == 0xd7 && bus.lutBytes == 49 && "menu FAST request was promoted to FULL");
  }

  // Each AA page may be followed by a fast B/W base, including after cleanup.
  for (bool cleanup : {false, true}) {
    for (int page = 0; page < 3; ++page) {
      driver.copyGrayscaleLsb(bus, gray.data());
      driver.copyGrayscaleMsb(bus, gray.data());
      const auto before = bus.refreshes;
      driver.displayGray(bus, bw.data(), false, nullptr, false);
      assert(bus.refreshes == before + 1 && bus.interval == 0x00 && bus.lutBytes == 49 &&
             "an AA gray pass must be one Gray3 refresh");
      if (cleanup) driver.cleanupGrayscaleBuffers(bus, bw.data());
      for (auto& b : bw) b = static_cast<uint8_t>(b ^ 0x33);
      driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, page == 2);
      assert(bus.interval == 0xd7 && bus.lutBytes == 49 && "post-AA FAST request was promoted to FULL");
      for (size_t i = 0; i < bytes; ++i) {
        const auto physical = (551 - i / 96) * 96 + i % 96;
        assert(bus.newAtRefresh[physical] == bw[i]);
        assert(bus.oldAtRefresh[physical] == static_cast<uint8_t>(~bw[i]));
      }
      assert(bus.oldPlane == bus.newPlane);
    }
  }

  // The caller still controls scheduled/manual cleanup.
  for (auto mode : {RefreshMode::Half, RefreshMode::Full}) {
    const auto refreshes = bus.refreshes;
    const auto powerOffs = bus.powerOffs;
    driver.display(bus, bw.data(), nullptr, mode, false);
    assert(bus.powerOffs - powerOffs == bus.refreshes - refreshes && "every refresh must power the panel off");
    assert(bus.interval == 0x97 && bus.lutBytes == 49);
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    assert(bus.interval == 0xd7 && bus.lutBytes == 49);
  }

  // Absolute planes go out as the stock firmware sends them: MSB -> DTM1,
  // LSB -> DTM2, uninverted, after stock's two-pass pre-clear to black.
  {
    std::vector<uint8_t> lsb(bytes), msb(bytes);
    for (size_t i = 0; i < bytes; ++i) {
      lsb[i] = static_cast<uint8_t>(i * 13);
      msb[i] = static_cast<uint8_t>(i * 29 + 7);
    }
    const auto before = bus.refreshes;
    driver.beginGrayscale(bus, bw.data(), GrayscaleMode::Direct, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    driver.displayGray(bus, bw.data(), false, nullptr, true);
    assert(bus.refreshes == before + 3 && "absolute gray pre-clears twice, then paints");
    // Stock's halved GC record 0x20 (V1.0.3 lockscreen pre-clear).
    const std::vector<uint8_t> half20 = {0x01, 0x0c, 0x02, 0x07, 0x05, 0x01, 0x01,
                                         0x01, 0x05, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01};
    bool sawHalf = false;
    for (const auto& r : bus.lut20s) sawHalf |= r.size() == 49 && std::equal(half20.begin(), half20.end(), r.begin());
    assert(sawHalf && "pre-clear must load stock's halved GC waveform");
    for (size_t i = 0; i < bytes; ++i) {
      const auto physical = (551 - i / 96) * 96 + i % 96;
      assert(bus.oldAtRefresh[physical] == msb[i] && bus.newAtRefresh[physical] == lsb[i]);
    }
  }

  // Gray3 (anti-aliased pages, stock's reader path). Byte 0 of row 0, bits 7..0:
  // base white, base black, dark, light, then white x4 -> levels white, black,
  // middle, middle, white x4. Codes (DTM1, DTM2): 10 black, 01 gray,
  // 11 white-clear, 00 hold.
  const auto physical0 = static_cast<size_t>(551) * 96;  // row 0 is sent last
  std::vector<uint8_t> page(bytes, 0xff), lsbMask(bytes, 0), msbMask(bytes, 0);
  page[0] = 0b10001111;
  lsbMask[0] = 0b00100000;  // dark
  msbMask[0] = 0b00110000;  // dark | light
  const auto gray3Page = [&](const uint8_t* base, RefreshMode fallback) {
    driver.beginGrayscale(bus, base, GrayscaleMode::Overlay, fallback, false);
    driver.writeGrayscalePlaneStrip(bus, GrayPlane::Lsb, lsbMask.data(), 0, 552);
    driver.writeGrayscalePlaneStrip(bus, GrayPlane::Msb, msbMask.data(), 0, 552);
    driver.displayGray(bus, base, false, nullptr, false);
  };
  {
    // First Gray3 page after a B/W screen: the base goes out as a normal B/W
    // refresh (clearing the previous screen), then a Gray3 transition from it
    // nudges only the gray pixels and re-drives black.
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    const auto before = bus.refreshes;
    gray3Page(page.data(), RefreshMode::Fast);
    assert(bus.refreshes == before + 2 && "entry must be one B/W base refresh plus one Gray3 transition");
    assert(bus.interval == 0x00 && bus.lutBytes == 49);
    assert(bus.oldAtRefresh[physical0] == 0b01000000 && bus.newAtRefresh[physical0] == 0b00110000 &&
           "from the B/W base: black re-driven (10), base-black -> middle nudged (01), white holds");
    assert(bus.oldAtRefresh[physical0 + 1] == 0x00 && bus.newAtRefresh[physical0 + 1] == 0x00);
    assert(bus.oldPlane.size() == 600 * 96 && bus.oldPlane[552 * 96] == 0x00 && "padding rows hold");
  }
  {
    // The same page again: one refresh; white and middle hold, black re-driven.
    const auto before = bus.refreshes;
    gray3Page(page.data(), RefreshMode::Fast);
    assert(bus.refreshes == before + 1);
    assert(bus.oldAtRefresh[physical0] == 0b01000000 && bus.newAtRefresh[physical0] == 0x00 &&
           "unchanged white/middle must hold, black re-driven");
    assert(bus.oldAtRefresh[physical0 + 1] == 0x00 && bus.newAtRefresh[physical0 + 1] == 0x00);
  }
  {
    // A page where those pixels change: middle -> white is cleared, white ->
    // middle is nudged gray, black -> white is cleared.
    std::vector<uint8_t> next(bytes, 0xff);
    next[0] = 0b11111111;  // all white base
    const auto savedLsb = lsbMask[0], savedMsb = msbMask[0];
    lsbMask[0] = 0;
    msbMask[0] = 0b10000000;  // bit 7 now middle
    gray3Page(next.data(), RefreshMode::Fast);
    // bit7 white->middle: 01; bit6 black->white: 11; bits5,4 middle->white: 11.
    assert(bus.oldAtRefresh[physical0] == 0b01110000 && bus.newAtRefresh[physical0] == 0b11110000);
    lsbMask[0] = savedLsb;
    msbMask[0] = savedMsb;
  }
  {
    // A cleanup request runs stock's inverted page first, then the page.
    const auto before = bus.refreshes;
    gray3Page(page.data(), RefreshMode::Half);
    assert(bus.refreshes == before + 2 && "cleanup page must show the inverted page, then the page");
  }
  {
    // A stashed base whose gray pass never runs still reaches the panel.
    driver.beginGrayscale(bus, page.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
    const auto before = bus.refreshes;
    driver.cleanupGrayscaleBuffers(bus, page.data());
    assert(bus.refreshes == before + 1 && bus.interval == 0xd7);
    for (size_t i = 0; i < bytes; ++i) assert(bus.newAtRefresh[(551 - i / 96) * 96 + i % 96] == page[i]);
  }
  {
    // A caller that shows the base with a plain display() and never announces
    // the overlay (image pages) still gets a Gray3 page on that base.
    driver.display(bus, page.data(), nullptr, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsbMask.data());
    driver.copyGrayscaleMsb(bus, msbMask.data());
    const auto before = bus.refreshes;
    driver.displayGray(bus, page.data(), false, nullptr, false);
    assert(bus.refreshes == before + 1 && bus.interval == 0x00 && bus.oldAtRefresh[physical0] == 0b01000000 &&
           bus.newAtRefresh[physical0] == 0b00110000 && "unannounced overlay must transition from the shown base");
  }

  driver.deepSleep(bus);
  driver.begin(bus);
  const auto wakeStart = bus.refreshes;
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.interval == 0x97 && bus.lutBytes == 49);
  assert(bus.refreshes == wakeStart + 2 && "first paint after wake must drive inverse, then target");
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.interval == 0xd7 && bus.lutBytes == 49);
  std::cout << "PASS: A4 FAST cadence, post-AA pixel transitions, retained baseline, explicit cleanup, four-gray and Gray3 wire encoding and wake\n";
}
