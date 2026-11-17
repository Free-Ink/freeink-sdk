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
      assert(bus.refreshes == before + 2 && bus.interval == 0x00 && bus.lutBytes == 49 &&
             "an AA gray pass must be one Gray3 refresh plus the dark refresh");
      if (cleanup) driver.cleanupGrayscaleBuffers(bus, bw.data());
      for (auto& b : bw) b = static_cast<uint8_t>(b ^ 0x33);
      driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, page == 2);
      assert(bus.vcom == 0x32 && "returning from grayscale must retain stock B/W VCOM");
      assert(bus.interval == 0xd7 && bus.lutBytes == 49 && "post-AA FAST request was promoted to FULL");
      for (size_t i = 0; i < bytes; ++i) {
        const auto physical = (551 - i / 96) * 96 + i % 96;
        assert(bus.newAtRefresh[physical] == bw[i]);
        // OLD is the gray page as shown (the previous frame, gray where masked),
        // with each gray pixel as the inverse of its target.
        const auto shownWhite = static_cast<uint8_t>((bw[i] ^ 0x33) & ~gray[i]);
        assert(bus.oldAtRefresh[physical] == static_cast<uint8_t>(shownWhite | (gray[i] & ~bw[i])));
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
  // base white, base black, dark, light, then white x4. First refresh codes
  // (DTM1, DTM2): 10 black, 01 gray, 11 white-clear, 00 hold. Second (dark)
  // refresh: 10 darker, 01 light from white, 11 dark from white, 00 hold.
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
    // refresh with its gray pixels white (clearing the previous screen), then
    // one Gray3 refresh drives light and dark from white; black holds.
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    const auto before = bus.refreshes;
    gray3Page(page.data(), RefreshMode::Fast);
    assert(bus.refreshes == before + 2 && "entry must be a B/W base refresh and one Gray3 refresh");
    assert(bus.interval == 0x00 && bus.lutBytes == 49);
    assert(bus.newAtPrevRefresh[physical0] == 0b10111111 && "the base goes out with its gray pixels white");
    assert(bus.oldAtRefresh[physical0] == 0b00100000 && bus.newAtRefresh[physical0] == 0b00010000 &&
           "from the B/W base: dark (10) and light (01) from white; white and black hold");
    assert(bus.oldAtRefresh[physical0 + 1] == 0x00 && bus.newAtRefresh[physical0 + 1] == 0x00);
    assert(bus.oldPlane.size() == 600 * 96 && bus.oldPlane[552 * 96] == 0x00 && "padding rows hold");
  }
  {
    // Idle retains Gray3 history/session. The same page again must resume with
    // one power-on and one refresh, with no register reinitialization.
    fakeMillis() += 500;
    driver.controllerIdle(bus);
    const auto ons = bus.powerOns, settings = bus.panelSettings;
    const auto before = bus.refreshes;
    gray3Page(page.data(), RefreshMode::Fast);
    assert(bus.refreshes == before + 1);
    assert(bus.powerOns == ons + 1 && bus.panelSettings == settings);
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
    // Light -> dark holds in the first refresh and is only driven further in
    // the second (10); nothing else changes.
    lsbMask[0] = 0b10000000;
    auto before = bus.refreshes;
    gray3Page(next.data(), RefreshMode::Fast);
    assert(bus.refreshes == before + 2);
    assert(bus.oldAtPrevRefresh[physical0] == 0x00 && bus.newAtPrevRefresh[physical0] == 0x00);
    assert(bus.oldAtRefresh[physical0] == 0b10000000 && bus.newAtRefresh[physical0] == 0x00);
    assert(bus.lut20s.back()[1] == 3 + 2 + 4 && "dark refresh: light + extra + settle frames");
    // Dark -> light is cleared (11), then driven from white (01).
    lsbMask[0] = 0;
    before = bus.refreshes;
    gray3Page(next.data(), RefreshMode::Fast);
    assert(bus.refreshes == before + 2);
    assert(bus.oldAtPrevRefresh[physical0] == 0b10000000 && bus.newAtPrevRefresh[physical0] == 0b10000000);
    assert(bus.oldAtRefresh[physical0] == 0x00 && bus.newAtRefresh[physical0] == 0b10000000);
    lsbMask[0] = savedLsb;
    msbMask[0] = savedMsb;
  }
  {
    // A cleanup request runs stock's inverted page first, then the page.
    const auto before = bus.refreshes;
    gray3Page(page.data(), RefreshMode::Half);
    assert(bus.refreshes == before + 3 &&
           "cleanup page must show the inverted page (with its dark refresh), then the page");
  }
  {
    // A stashed base whose gray pass never runs still reaches the panel.
    driver.beginGrayscale(bus, page.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
    const auto before = bus.refreshes;
    const auto commands = bus.commands.size();
    driver.controllerIdle(bus);
    assert(bus.commands.size() == commands && "idle must not interrupt a pending grayscale base");
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
    assert(bus.refreshes == before + 2 && bus.interval == 0x00 && bus.newAtPrevRefresh[physical0] == 0b10111111 &&
           bus.oldAtRefresh[physical0] == 0b00100000 && bus.newAtRefresh[physical0] == 0b00010000 &&
           "unannounced overlay: the shown base is repainted with its grays white, then one Gray3 refresh");
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

    // A page with a pixel going light -> dark takes a second, short refresh.
    // The hook runs during the first one only.
    {
      const auto savedLsb = lsbMask[0];
      lsbMask[0] = 0b00110000;  // bit 4: light -> dark
      before = bus.refreshes;
      driver.setRefreshIdleHook(hook, &calls);
      gray3Page(page.data(), RefreshMode::Fast);
      assert(bus.refreshes == before + 2 && calls.count == 4 && calls.refreshesAtCall == before + 1 &&
             "with a second gray refresh the hook runs once, during the first");
      lsbMask[0] = savedLsb;
    }
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
    // Idle is POWER_OFF only, not deep sleep: registers and both RAM planes
    // survive, and each subsequent paint is still a differential FAST refresh.
    assert(bus.lut24.size() == 49 && bus.lut24[1] == 0x64 &&
           "FAST must re-drive unchanged black pixels, matching stock");
    for (int turn = 0; turn < 3; ++turn) {
      const auto shown = bus.oldPlane, newPlane = bus.newPlane;
      const auto offs = bus.powerOffs, ons = bus.powerOns, sleeps = bus.deepSleeps;
      const auto settings = bus.panelSettings;
      const auto commandCount = bus.commands.size();
      const auto refreshedAt = fakeMillis();
      driver.controllerIdle(bus);
      assert(bus.commands.size() == commandCount && "idle must not power off immediately after a refresh");
      fakeMillis() = refreshedAt + 498;  // millis() advances once per call in this harness
      driver.controllerIdle(bus);
      assert(bus.commands.size() == commandCount && "idle must retain power through 499 ms");
      driver.controllerIdle(bus);  // exactly 500 ms since the completed refresh
      driver.controllerIdle(bus);
      assert(bus.powerOffs == offs + 1 && bus.deepSleeps == sleeps);
      assert(bus.commands.size() == commandCount + 1 && bus.commands.back() == 0x02 &&
             "idle must send only POWER_OFF, once");
      assert(bus.oldPlane == shown && bus.newPlane == newPlane && "idle must preserve RAM");
      for (auto& b : bw) b = static_cast<uint8_t>(b ^ 0x0f);
      const auto before = bus.refreshes;
      driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
      assert(bus.refreshes == before + 1 && bus.interval == 0xd7 && "paint after idle stays one FAST refresh");
      assert(bus.powerOns == ons + 1 && bus.panelSettings == settings &&
             "paint after idle must power on without reinitializing registers");
      assert(bus.oldAtRefresh == shown && "paint after idle must diff against the displayed frame");
    }
    const auto offs = bus.powerOffs, sleeps = bus.deepSleeps;
    fakeMillis() += 500;
    driver.controllerIdle(bus);
    driver.deepSleep(bus);
    assert(bus.powerOffs == offs + 1 && "deep sleep after idle sends no second power-off");
    assert(bus.deepSleeps == sleeps + 1 && "real sleep still sends DEEP_SLEEP after idle");
    driver.begin(bus);
    const auto before = bus.refreshes;
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
    assert(bus.refreshes == before + 2 && bus.interval == 0x97 &&
           "real sleep/wake must still establish an unknown-glass baseline");
  }
  {
    // Gray3 codes against a per-pixel reference, over random page turns. Each
    // pixel has a level (0 white, 1 light, 2 dark, 3 black); the code of each
    // refresh depends on the level shown and the new one.
    const auto planeBit = [&](const std::vector<uint8_t>& plane, const size_t pixel) {
      const size_t i = pixel / 8;
      return (plane[(551 - i / 96) * 96 + i % 96] >> (7 - pixel % 8)) & 1;
    };
    const auto assertPaddingHolds = [&](const std::vector<uint8_t>& plane) {
      assert(plane.size() == 600 * 96);
      for (size_t i = bytes; i < plane.size(); ++i) assert(plane[i] == 0x00 && "padding rows must hold");
    };
    uint32_t seed = 0x2545f491;
    const auto next = [&] {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };
    std::vector<uint8_t> base(bytes), msb(bytes), lsb(bytes), shown(bytes * 8), level(bytes * 8);
    bool history = false;  // the last refresh above was B/W
    unsigned twoRefreshTurns = 0;
    for (int turn = 0; turn < 12; ++turn) {
      for (size_t i = 0; i < bytes; ++i) {
        base[i] = static_cast<uint8_t>(next() >> 8);
        // Sparse grays on most pages, none on one, so both LUT variants and the
        // one-refresh turn are covered. The dark plane also has bits outside
        // the gray mask, which must be ignored.
        msb[i] = turn == 5 ? 0 : static_cast<uint8_t>((next() >> 8) & (next() >> 8));
        lsb[i] = static_cast<uint8_t>(next() >> 8);
        for (unsigned bit = 0; bit < 8; ++bit) {
          const uint8_t mask = static_cast<uint8_t>(0x80 >> bit);
          level[i * 8 + bit] = msb[i] & mask ? (lsb[i] & mask ? 2 : 1) : (base[i] & mask ? 0 : 3);
        }
      }
      const auto before = bus.refreshes;
      driver.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
      driver.copyGrayscaleLsb(bus, lsb.data());
      driver.copyGrayscaleMsb(bus, msb.data());
      driver.displayGray(bus, base.data(), false, nullptr, false);
      driver.cleanupGrayscaleBuffers(bus, base.data());
      if (!history) {
        // From the B/W base: one Gray3 refresh, light 01 and dark 10 from white.
        assert(bus.refreshes == before + 2);
        assertPaddingHolds(bus.oldAtRefresh);
        assertPaddingHolds(bus.newAtRefresh);
        for (size_t px = 0; px < bytes * 8; ++px) {
          assert(planeBit(bus.oldAtRefresh, px) == (level[px] == 2));
          assert(planeBit(bus.newAtRefresh, px) == (level[px] == 1));
        }
      } else {
        bool second = false;
        for (size_t px = 0; px < bytes * 8 && !second; ++px) {
          const bool gray = level[px] == 1 || level[px] == 2;
          second = gray && (shown[px] == 3 || (shown[px] == 2 && level[px] == 1) || (shown[px] != 2 && level[px] == 2));
        }
        assert(bus.refreshes == before + (second ? 2 : 1));
        twoRefreshTurns += second;
        const auto& old1 = second ? bus.oldAtPrevRefresh : bus.oldAtRefresh;
        const auto& new1 = second ? bus.newAtPrevRefresh : bus.newAtRefresh;
        assertPaddingHolds(old1);
        assertPaddingHolds(new1);
        if (second) {
          assertPaddingHolds(bus.oldAtRefresh);
          assertPaddingHolds(bus.newAtRefresh);
        }
        for (size_t px = 0; px < bytes * 8; ++px) {
          const uint8_t was = shown[px], now = level[px];
          const bool gray = now == 1 || now == 2;
          // A gray pixel that was darker is cleared first and driven from white second.
          const bool regray = gray && (was == 3 || (was == 2 && now == 1));
          unsigned first = 0, later = 0;  // (DTM1, DTM2) codes
          if (now == 3) first = 0b10;
          else if (now == 0) first = was == 0 ? 0b00 : 0b11;
          else if (regray) first = 0b11;
          else first = was == 0 ? 0b01 : 0b00;
          if (regray) later = now == 2 ? 0b11 : 0b01;
          else if (now == 2 && was != 2) later = 0b10;
          assert(static_cast<unsigned>(planeBit(old1, px) << 1 | planeBit(new1, px)) == first);
          const unsigned sent = second ? planeBit(bus.oldAtRefresh, px) << 1 | planeBit(bus.newAtRefresh, px) : 0;
          assert(sent == later);
        }
      }
      shown = level;
      history = true;
    }
    assert(twoRefreshTurns >= 5 && "the random pages must exercise the second refresh");
  }
  std::cout << "PASS: A4 FAST cadence, post-AA pixel transitions, retained baseline, explicit cleanup, four-gray and "
               "Gray3 wire encoding, refresh idle hook, stock power hold and wake, idle power-off, Gray3 codes against "
               "a reference\n";
}
