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
  assert(bus.vcom == 0x32 && "B/W initialization must retain stock VCOM for idle stability");

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
      assert(bus.vcom == 0x32 && "returning from grayscale must retain stock B/W VCOM");
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
    for (auto& b : bw) b = static_cast<uint8_t>(b ^ 0x99);  // a different screen, so OLD differs from NEW
    driver.display(bus, bw.data(), nullptr, mode, false);
    assert(bus.refreshes > refreshes && bus.powerOffs == powerOffs &&
           "B/W refreshes keep the panel powered, as stock's power hold does");
    assert(bus.interval == 0x97 && bus.lutBytes == 49);
    assert(bus.oldAtRefresh == bus.newAtRefresh &&
           "complete refreshes must put every pixel on the flashing same-state records");
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
    const std::vector<uint8_t> half20 = {0x01, 0x0c, 0x02, 0x07, 0x05, 0x01, 0x01, 0x01,
                                         0x05, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01};
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

  {
    // Gray3 page turns keep the panel powered, as stock's power hold does. The
    // next B/W refresh powers the held panel off only to reset the controller.
    const auto offs = bus.powerOffs;
    gray3Page(page.data(), RefreshMode::Fast);
    gray3Page(page.data(), RefreshMode::Fast);
    assert(bus.powerOffs == offs && "Gray3 page turns must not power off between pages");
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    assert(bus.powerOffs == offs + 1 && "B/W after Gray3: power off the held panel for the reset only");
  }

  {
    // Refresh idle hook: one-shot host work during a Gray3 page-turn waveform.
    struct Calls {
      EpdBus* bus;
      unsigned count = 0, refreshesAtCall = 0;
    } calls{&bus};
    const auto hook = [](void* ctx) {
      auto* c = static_cast<Calls*>(ctx);
      ++c->count;
      c->refreshesAtCall = c->bus->refreshes;
    };
    gray3Page(page.data(), RefreshMode::Fast);  // back in a Gray3 session
    bus.busyPollsPerRefresh = 1000;             // still busy when the hook returns

    auto before = bus.refreshes;
    auto waits = bus.busyWaits;
    driver.setRefreshIdleHook(hook, &calls);
    gray3Page(page.data(), RefreshMode::Fast);
    assert(calls.count == 1 && calls.refreshesAtCall == before + 1 && "the hook runs once, after the refresh is fired");
    assert(bus.busyWaits == waits + 1 && "a waveform still running after the hook is waited out");
    gray3Page(page.data(), RefreshMode::Fast);
    assert(calls.count == 1 && "the hook is one-shot");

    // The waveform ends during the hook: no second wait, which would only time out.
    bus.busyPollsPerRefresh = 1;
    waits = bus.busyWaits;
    driver.setRefreshIdleHook(hook, &calls);
    gray3Page(page.data(), RefreshMode::Fast);
    assert(calls.count == 2 && bus.busyWaits == waits && "a refresh that finished during the hook is not waited on");

    // BUSY never asserts: the hook is dropped, not run late, and the normal wait runs.
    bus.busyPollsPerRefresh = 0;
    waits = bus.busyWaits;
    driver.setRefreshIdleHook(hook, &calls);
    gray3Page(page.data(), RefreshMode::Fast);
    bus.busyPollsPerRefresh = 1000;
    gray3Page(page.data(), RefreshMode::Fast);
    assert(calls.count == 2 && bus.busyWaits == waits + 2 && "a hook whose waveform never started must not run");

    // The hook lasts for one display call. A B/W refresh never runs it and
    // still ends it, so it cannot fire on a later page turn.
    driver.setRefreshIdleHook(hook, &calls);
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    assert(calls.count == 2 && "the hook must not run during a B/W refresh");
    gray3Page(page.data(), RefreshMode::Fast);
    assert(calls.count == 2 && "a hook set before a B/W refresh must not survive it");

    // Nor does a four-gray image.
    {
      std::vector<uint8_t> planes(bytes, 0x5a);
      driver.beginGrayscale(bus, bw.data(), GrayscaleMode::Direct, RefreshMode::Half, false);
      driver.copyGrayscaleLsb(bus, planes.data());
      driver.copyGrayscaleMsb(bus, planes.data());
      driver.setRefreshIdleHook(hook, &calls);
      driver.displayGray(bus, bw.data(), false, nullptr, true);
      assert(calls.count == 2 && "the hook must not run during a four-gray refresh");
      gray3Page(page.data(), RefreshMode::Fast);
      assert(calls.count == 2 && "a hook set before a four-gray refresh must not survive it");
    }

    // First text page after a B/W screen: the base goes out as a B/W refresh
    // inside the same call, and the hook still runs during the Gray3 refresh.
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    before = bus.refreshes;
    driver.beginGrayscale(bus, page.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
    driver.writeGrayscalePlaneStrip(bus, GrayPlane::Lsb, lsbMask.data(), 0, 552);
    driver.writeGrayscalePlaneStrip(bus, GrayPlane::Msb, msbMask.data(), 0, 552);
    driver.setRefreshIdleHook(hook, &calls);
    driver.displayGray(bus, page.data(), false, nullptr, false);
    assert(bus.refreshes == before + 2 && calls.count == 3 && calls.refreshesAtCall == before + 2 &&
           "the hook runs during the Gray3 refresh, not the B/W base before it");

    // A hook cleared by the host does not run.
    driver.setRefreshIdleHook(hook, &calls);
    driver.setRefreshIdleHook(nullptr, nullptr);
    gray3Page(page.data(), RefreshMode::Fast);
    assert(calls.count == 3 && "a cleared hook must not run");
    bus.busyPollsPerRefresh = 0;
  }

  {
    // Deep sleep powers a held panel off.
    const auto offs = bus.powerOffs;
    driver.deepSleep(bus);
    assert(bus.powerOffs == offs + 1 && "deep sleep must power the held panel off");
  }
  driver.begin(bus);
  driver.begin(bus);
  const auto wakeStart = bus.refreshes;
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.interval == 0x97 && bus.lutBytes == 49);
  assert(bus.refreshes == wakeStart + 2 && "first paint after wake must drive inverse, then target");
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.interval == 0xd7 && bus.lutBytes == 49);
  {
    // controllerIdle() powers the panel off between frames; the next paint
    // wakes the controller and diffs against the frame left on the glass.
    const auto shown = bus.oldPlane;
    const auto offs = bus.powerOffs;
    driver.controllerIdle(bus);
    driver.controllerIdle(bus);
    assert(bus.powerOffs == offs + 1 && "idle must power off once and stay off");
    for (auto& b : bw) b = static_cast<uint8_t>(b ^ 0x0f);
    const auto before = bus.refreshes;
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    assert(bus.refreshes == before + 1 && bus.interval == 0xd7 && "paint after idle stays one FAST refresh");
    assert(bus.oldAtRefresh == shown && "paint after idle must diff against the displayed frame");
    driver.controllerIdle(bus);
    driver.deepSleep(bus);
    assert(bus.powerOffs == offs + 2 && "deep sleep after idle sends no second power-off");
  }
  std::cout << "PASS: A4 FAST cadence, post-AA pixel transitions, retained baseline, explicit cleanup, four-gray and "
               "Gray3 wire encoding, refresh idle hook, stock power hold and wake, idle power-off\n";
}
