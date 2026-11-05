// Onyx Picco: panel-ID probe picks the stock per-panel SSD1677 tables.
// Run once per panel (the driver is a process-wide singleton): yrd | depg | opm | unknown.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "esp_heap_caps.h"
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/driver/Ssd2677Driver.h"
#include "src/lut/Ssd1677Luts.h"
#include "src/lut/Ssd2677Luts.h"
#undef private

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static uint8_t lastRegister(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command && !i->bytes.empty()) return i->bytes[0];
  assert(false);
  return 0;
}

static const Bytes* lastLut(const EpdBus& bus) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == 0x32) return &i->bytes;
  return nullptr;
}

static void expectRefresh(Ssd1677Driver& d, RefreshMode mode, uint8_t ctrl2, const unsigned char* lut) {
  EpdBus bus;
  const Bytes fb(48000, 0xAA);
  d.display(bus, fb.data(), fb.data(), mode, false);
  assert(lastRegister(bus, 0x22) == ctrl2);
  if (d._cfg.piccoWaveforms) assert(d._isScreenOn == ((ctrl2 & 0x03) == 0));
  const Bytes* sent = lastLut(bus);
  if (lut) {
    assert(sent && *sent == Bytes(lut, lut + 105));
    assert(lastRegister(bus, 0x2C) == lut[109]);
  } else {
    assert(!sent);
  }
  assert(!d._customLutActive);
}

// Sticky AA keeps its voltage/activation sequence and BOOX gray-aware history.
static void expectPiccoGray(Ssd1677Driver& d) {
  EpdBus bus;
  const Bytes base(48000, 0x55), lsb(48000, 0x11), msb(48000, 0x33);
  d.begin(bus);
  d.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  d.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  d.copyGrayscaleLsb(bus, lsb.data());
  d.writeGrayscalePlaneStrip(bus, GrayPlane::Msb, msb.data(), 0, 480);
  bus.clear();
  const bool grayNeedsPowerOn = !d._isScreenOn;
  d.displayGray(bus, msb.data(), false, nullptr, false);
  assert(lastLut(bus) && *lastLut(bus) == Bytes(d._cfg.grayLut, d._cfg.grayLut + 105));
  assert(d._cfg.grayLut == lut_grayscale_sticky);
  assert(lastRegister(bus, 0x22) == 0xCF && d._piccoGrayPending);
  assert(lastRegister(bus, 0x03) == lut_grayscale_sticky[105]);
  assert(lastRegister(bus, 0x2C) == lut_grayscale_sticky[109]);
  assert(lastRegister(bus, 0x3C) == 0x80);
  unsigned activations = 0;
  for (const auto& w : bus.writes) {
    activations += w.command == 0x20;
  }
  assert(activations == (grayNeedsPowerOn ? 2u : 1u));
  std::vector<uint8_t> aaSequences;
  for (const auto& w : bus.writes)
    if (w.command == 0x22) aaSequences.push_back(w.bytes.at(0));
  assert(aaSequences == (grayNeedsPowerOn ? std::vector<uint8_t>{0xC0, 0xCF}
                                         : std::vector<uint8_t>{0xCF}));
  d.cleanupGrayscaleBuffers(bus, base.data());
  bus.clear();
  // Same B/W base, but gray pixels must transition instead of being held.
  assert(d.displayStart(bus, base.data(), base.data(), RefreshMode::Fast, false));
  activations = 0;
  for (const auto& w : bus.writes) {
    if (w.command == 0x26) assert(w.bytes == Bytes(48000, 0x66));
    activations += w.command == 0x20;
  }
  assert(activations == 1 && bus.waits == 0);
  assert(lastRegister(bus, 0x3C) == d._cfg.borderWaveformInit);
  d.displayFinish(bus, base.data());
  assert(Bytes(d._piccoOld, d._piccoOld + 48000) == base);
  bus.clear();
  d.display(bus, base.data(), nullptr, RefreshMode::Fast, false);
  for (const auto& w : bus.writes)
    if (w.command == 0x26) assert(w.bytes == base);  // mask is consumed once
  bus.clear();
  d.beginGrayscale(bus, base.data(), GrayscaleMode::Absolute, RefreshMode::Half, false);
  d.copyGrayscaleLsb(bus, lsb.data());
  d.copyGrayscaleMsb(bus, msb.data());
  for (const auto& w : bus.writes) {
    if (w.command == 0x24) assert(w.bytes == Bytes(48000, 0xEE));
    if (w.command == 0x26) assert(w.bytes == Bytes(48000, 0xCC));
  }
  bus.clear();
  d.displayGray(bus, msb.data(), true, nullptr, true);
  assert(lastLut(bus) && *lastLut(bus) == Bytes(lut_factory_quality, lut_factory_quality + 105));
  assert(lastRegister(bus, 0x03) == lut_factory_quality[105]);
  assert(lastRegister(bus, 0x04) == lut_factory_quality[106]);
  assert(lastRegister(bus, 0x2C) == lut_factory_quality[109]);
  assert(lastRegister(bus, 0x3C) == d._cfg.borderWaveformInit);
  std::vector<uint8_t> graySequences;
  for (const auto& w : bus.writes)
    if (w.command == 0x22) graySequences.push_back(w.bytes.at(0));
  assert((graySequences == std::vector<uint8_t>{0xCC, 0x03}));
  assert(d._needsGrayClear && !d._isScreenOn);
  // The first charge-strip paint after an absolute image retains main's recovery sequence.
  bus.clear();
  d.display(bus, msb.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x22) == (d._cfg.halfLut ? 0xC4 : 0xD7));
  if (d._cfg.halfLut) assert(lastRegister(bus, 0x3C) == 0x80);
  unsigned recoveryActivations = 0, bwWrites = 0, oldWrites = 0;
  for (const auto& w : bus.writes) {
    recoveryActivations += w.command == 0x20;
    bwWrites += w.command == 0x24;
    oldWrites += w.command == 0x26;
  }
  assert(recoveryActivations == 1 && bwWrites == 2 && oldWrites == 2);
  assert(!d._needsGrayClear);
  // No PSRAM: retain correct output by promoting the next request, never use stale gray RAM.
  Ssd1677Driver noHistory(d._cfg);
  fakeHeapCapsFail = true;
  noHistory.begin(bus);
  fakeHeapCapsFail = false;
  assert(!noHistory._piccoOld && !noHistory._piccoGrayMask);
  noHistory.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  noHistory.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
  noHistory.copyGrayscaleLsb(bus, lsb.data());
  noHistory.copyGrayscaleMsb(bus, msb.data());
  noHistory.displayGray(bus, msb.data(), false, nullptr, false);
  noHistory.cleanupGrayscaleBuffers(bus, base.data());
  assert(noHistory._needsGrayClear);
  bus.clear();
  noHistory.display(bus, base.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x21) == 0x40);
}

static void expectRetainedGrayStrip(Ssd1677Driver& d) {
  EpdBus bus;
  Bytes bw(48000, 0xFF), lsb(48000, 0x55), msb(48000, 0x33);
  d.begin(bus);
  assert(!d.displayRetainedGrayscaleOverlay(bus, bw.data(), 3, 2, 11, 3));
  d.display(bus, bw.data(), nullptr, RefreshMode::Full, false);
  d.beginGrayscale(bus, bw.data(), GrayscaleMode::Absolute, RefreshMode::Half, false);
  d.copyGrayscaleLsb(bus, lsb.data());
  d.writeGrayscalePlaneStrip(bus, GrayPlane::Msb, msb.data(), 0, 480);
  d.displayGray(bus, msb.data(), true, nullptr, true);
  d.requestResync(1);  // facade finishes an absolute session this way
  for (int paint = 0; paint < 3; ++paint) {
    std::fill(bw.begin(), bw.end(), paint == 0 ? 0xF0 : 0x0F);
    const bool restore = paint == 2;
    bus.clear();
    assert(d.displayRetainedGrayscaleOverlay(bus, restore ? nullptr : bw.data(), 3, 2, 11, 3));
    unsigned activations = 0, planes = 0;
    for (const auto& w : bus.writes) {
      activations += w.command == 0x20;
      if (w.command != 0x24 && w.command != 0x26) continue;
      ++planes;
      assert(w.bytes.size() == 48000);
      const auto& original = w.command == 0x24 ? lsb : msb;
      for (size_t i = 0; i < original.size(); ++i) {
        uint8_t expected = original[i];
        if (!restore && i / 100 >= 2 && i / 100 < 5) {
          for (int bit = 0; bit < 8; ++bit) {
            const int x = (i % 100) * 8 + bit;
            if (x >= 3 && x < 14) {
              const uint8_t mask = 0x80 >> bit;
              expected = (expected & ~mask) | (bw[i] & mask);
            }
          }
        }
        if (!d._cfg.piccoStockGrayscale) expected = ~expected;
        assert(w.bytes[i] == expected);
      }
    }
    // Factory gray powers down separately after its single image activation.
    assert(planes == 2 && activations == (d._cfg.piccoStockGrayscale ? 1u : 2u));
  }
  bus.clear();
  assert(!d.displayRetainedGrayscaleOverlay(bus, bw.data(), 799, 0, 2, 1));
  assert(bus.writes.empty());
  d.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  bus.clear();
  assert(!d.displayRetainedGrayscaleOverlay(bus, nullptr, 3, 2, 11, 3));
  assert(bus.writes.empty());
}

static void expectStockPiccoGray(Ssd1677Driver& d) {
  EpdBus bus;
  Bytes base(48000, 0x55), lsb(48000, 0x11), msb(48000, 0x33);
  d.begin(bus);
  d.display(bus, base.data(), nullptr, RefreshMode::Full, false);
  for (bool absolute : {false, true}) {
    d.beginGrayscale(bus, base.data(), absolute ? GrayscaleMode::Absolute : GrayscaleMode::Overlay,
                     RefreshMode::Fast, false);
    bus.clear();
    d.copyGrayscaleLsb(bus, lsb.data());
    d.writeGrayscalePlaneStrip(bus, GrayPlane::Msb, msb.data(), 0, 480);
    for (const auto& w : bus.writes) {
      if (w.command == 0x24) assert(w.bytes == lsb);
      if (w.command == 0x26) assert(w.bytes == msb);
    }
    bus.clear();
    const bool wasOn = d._isScreenOn;
    d.displayGray(bus, msb.data(), absolute, nullptr, absolute);
    const auto* lut = absolute ? d._cfg.factoryGrayLut : lut_picco_yrd_opm_gray;
    if (lut) assert(lastLut(bus) && *lastLut(bus) == Bytes(lut, lut + 105));
    else assert(!lastLut(bus));
    uint8_t sequence = lut ? 0xC7 : 0xD7;
    if (wasOn) sequence &= 0x3F;
    // Both reader AA and this turnOff image request power down.
    assert(lastRegister(bus, 0x22) == sequence);
    unsigned activations = 0;
    for (const auto& w : bus.writes) {
      assert(w.command != 0x03 && w.command != 0x04 && w.command != 0x2C);
      activations += w.command == 0x20;
    }
    assert(activations == 1);
    if (!absolute) {
      d.cleanupGrayscaleBuffers(bus, base.data());
      bus.clear();
      d.display(bus, base.data(), nullptr, RefreshMode::Fast, false);
      for (const auto& w : bus.writes)
        if (w.command == 0x26) assert(w.bytes == Bytes(48000, 0x66));
    } else assert(d._needsGrayClear && !d._isScreenOn);
  }
}

// A complete frame can span an async base and a grayscale overlay. Idle
// shutdown must retain RAM/state, and the next page must remain partial.
static void expectIdlePower(Ssd1677Driver& d) {
  FreeInkDisplay frame(1, 2, 3, 4, 5, 6);
  frame.selectDriver();
  auto& bus = frame._bus;
  const Bytes fb(48000, 0xAA);
  d.begin(bus);
  d.display(bus, fb.data(), nullptr, RefreshMode::Full, false);
  bus.clear();
  assert(d.supportsAsyncDisplay());
  assert(d.displayStart(bus, fb.data(), fb.data(), RefreshMode::Fast, false));
  const uint8_t startedSequence = lastRegister(bus, 0x22);
  const bool needsShutdown = (startedSequence & 0x03) == 0;
  assert(d._isScreenOn == needsShutdown);
  assert(bus.waits == 0);
  frame._refreshPending = true;
  fakeDelayMs = 0;
  frame.powerOffBetweenFrames();
  assert(!frame._refreshPending && !d._isScreenOn && !d._needsInitialFull);
  assert(lastRegister(bus, 0x22) == (needsShutdown ? 0x03 : startedSequence));
  assert(bus.waits == (needsShutdown ? 2u : 1u));
  assert(fakeDelayMs == (needsShutdown ? 1u : 0u));  // command propagation only; never the 200 ms helper delay
  for (const auto& w : bus.writes) assert(w.command != 0x10 && w.command != 0x12);  // no sleep/reset
  bus.clear();
  frame.powerOffBetweenFrames();
  assert(bus.writes.empty());
  assert(d.displayStart(bus, fb.data(), fb.data(), RefreshMode::Fast, false));
  assert(lastRegister(bus, 0x22) == startedSequence);
  d.displayFinish(bus, fb.data());
  // Reader AA powers off; RAM cleanup and repeated idle shutdown remain safe.
  d.displayGray(bus, fb.data(), false, nullptr, false);
  assert(d._isScreenOn == !d._cfg.piccoWaveforms);
  d.cleanupGrayscaleBuffers(bus, fb.data());
  frame.powerOffBetweenFrames();
  assert(!d._isScreenOn && !d._inGrayscaleMode && !d._needsInitialFull);
  d.displayGray(bus, fb.data(), false, nullptr, true);
  frame.powerOffBetweenFrames();
  assert(!d._isScreenOn && d._needsGrayClear);
}

int main(int argc, char** argv) {
  const char* which = argc > 1 ? argv[1] : "yrd";
  static const uint8_t yrd[10] = {0xCA, 0xFE, 0x00, 0x16, 0x77, 0x02, 0x10, 0x01, 0x00, 0x91};
  static const uint8_t depg[10] = {0xCA, 0xFE, 0x00, 0x16, 0x77, 0x00, 0x08, 0x01, 0x00, 0x4E};
  static const uint8_t opm[10] = {'W', '0', '4', '0', 'B', '3', 0, 0, 0, 0};
  static const uint8_t none[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t* id = !strcmp(which, "depg")      ? depg
                      : !strcmp(which, "opm")     ? opm
                      : !strcmp(which, "unknown") ? none
                                                  : yrd;
  memcpy(fakePanelId, id, 10);
  const auto checkController = [](const char* expected) {
    FreeInkDisplay display(1, 2, 3, 4, 5, 6);
    const int before = fakePanelCommand;
    assert(display.getControllerName() == nullptr && fakePanelCommand == before);
    display.selectDriver();
    assert(!strcmp(display.getControllerName(), expected));
    const int after = fakePanelCommand;
    assert(!strcmp(display.getControllerName(), expected) && fakePanelCommand == after);
  };
  if (!strcmp(which, "se0400")) {
    static const uint8_t rev[3] = {0x07, 0x01, 0x01};
    memcpy(fakePanelId, none, 10);
    memcpy(fakePanelRev, rev, 3);
    checkController("SSD2677");
    assert(piccoProbePanel() == PiccoSe0400nqw47);
    auto& s = static_cast<Ssd2677Driver&>(ssd2677Driver());
    assert(s.busyPolarity() == BusyPolarity::ActiveLow);
    EpdBus bus;
    s.begin(bus);
    assert(lastRegister(bus, 0x00) == 0x27 && lastRegister(bus, 0xE9) == 0x01);
    // Picco's final init command has no per-command wait: keep the final
    // completion wait as well as the reset wait.
    assert(bus.busyWaitWriteCounts == std::vector<size_t>({0, 12}));
    Bytes oldFb(48000, 0xFF), newFb(48000, 0xFF);
    newFb[0] = 0x0F;  // left 4 px black, right 4 px white; old all white
    bus.clear();
    s.display(bus, newFb.data(), oldFb.data(), RefreshMode::Fast, false);
    // First paint is forced FULL; both planes contain the absolute target.
    const Bytes* data = nullptr;
    for (const auto& w : bus.writes)
      if (w.command == 0x10) data = &w.bytes;
    assert(data && data->size() == 96000);
    assert((*data)[0] == 0x00 && (*data)[1] == 0xFF);  // black (0,0)x4 then white (1,1)x4
    const Bytes* lut = nullptr;
    for (const auto& w : bus.writes)
      if (w.command == 0x20) lut = &w.bytes;
    assert(lut && *lut == Bytes(lut_picco_se0400_full, lut_picco_se0400_full + 535));
    assert(lastRegister(bus, 0x12) == 0x00);
    bus.clear();
    s.display(bus, newFb.data(), oldFb.data(), RefreshMode::Fast, true);
    for (const auto& w : bus.writes)
      if (w.command == 0x20) lut = &w.bytes;
    assert(*lut == Bytes(lut_picco_se0400_mode3, lut_picco_se0400_mode3 + 535));
    assert(!s._needsPowerOn);
    for (const auto& w : bus.writes) assert(w.command != 0x02);
    // A second UI update reuses the powered panel, even when the host requests off.
    bus.clear();
    s.display(bus, newFb.data(), nullptr, RefreshMode::Fast, true);
    unsigned uiRefreshes = 0;
    for (const auto& w : bus.writes) {
      assert(w.command != 0x04 && w.command != 0x02);
      if (w.command == 0x12) ++uiRefreshes;
    }
    assert(uiRefreshes == 1 && !s._needsPowerOn);
    bus.clear();
    s.powerOffBetweenFrames(bus);
    assert(lastRegister(bus, 0x02) == 0 && s._needsPowerOn);
    s.setReaderRefresh(true);
    bus.clear();
    s.display(bus, newFb.data(), oldFb.data(), RefreshMode::Fast, true);
    data = nullptr;
    for (const auto& w : bus.writes) {
      if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_fast, lut_picco_se0400_fast + 535));
      if (w.command == 0x10) data = &w.bytes;
    }
    // Reader mode retains unchanged black pixels instead of applying the UI top-up.
    assert(data && data->size() == 96000 && (*data)[0] == 0x00 && (*data)[1] == 0xFF);
    s.setReaderRefresh(false);
    assert(lastRegister(bus, 0x02) == 0x00);

    // Regression (single-buffer ghosting): the host builds with
    // EINK_DISPLAY_SINGLE_BUFFER_MODE, so `prev` is ALWAYS null. The driver must
    // use its OWN retained old plane — seeded white in begin (stock's 0xFF
    // obj+0x1020 fill, FUN_4200b07c), then updated to each displayed frame — or
    // every incremental update sees old==new and moves nothing (the whole screen
    // ghosts). Paint black (full), then white (fast), both with prev=null: the
    // fast frame's old plane must be the RETAINED black, giving (old=0,new=1)=0b01
    // => 0x55 bytes. The old==new bug produced 0xFF.
    s.begin(bus);  // re-seeds _old = white, arms the forced initial full
    Bytes black(48000, 0x00), white(48000, 0xFF);
    s.display(bus, black.data(), nullptr, RefreshMode::Full, false);  // _old := black
    bus.clear();
    s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);  // old must = retained black
    data = nullptr;
    for (const auto& w : bus.writes)
      if (w.command == 0x10) data = &w.bytes;
    assert(data && data->size() == 96000);
    assert((*data)[0] == 0x55 && (*data)[100] == 0x55 && (*data)[95999] == 0x55);
    // Overlay AA: LSB mask -> low bit, MSB mask -> high bit, slot-5 LUT.
    assert(s.grayscaleCapabilities(GrayscaleMode::Overlay).supported());
    assert(s.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
    // Black, dark, light, white, repeated across the complete slot-4 frame.
    Bytes imageLsb(48000, 0x55), imageMsb(48000, 0x33);
    bus.clear();
    s.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, RefreshMode::Half, false);
    s.copyGrayscaleLsb(bus, imageLsb.data());
    s.copyGrayscaleMsb(bus, imageMsb.data());
    assert(bus.writes.empty() && !s._needsPowerOn);
    s.requestResync(1);  // the facade cancels a staged absolute image
    assert(!s._absoluteGray && s._needsInitialFull && bus.writes.empty());
    s.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, RefreshMode::Half, false);
    s.copyGrayscaleLsb(bus, imageLsb.data());
    s.copyGrayscaleMsb(bus, imageMsb.data());
    std::fill(imageMsb.begin(), imageMsb.end(), 0);  // caller reuses the upload buffer
    s.displayGray(bus, imageMsb.data(), true, nullptr, true);
    std::vector<uint8_t> imageCommands;
    for (const auto& w : bus.writes) {
      imageCommands.push_back(w.command);
      if (w.command == 0x10) assert(w.bytes == Bytes(96000, 0x1B));
      if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_image, lut_picco_se0400_image + 535));
    }
    assert((imageCommands == std::vector<uint8_t>{0x10, 0x20, 0x12, 0x02}));
    assert(s._needsInitialFull && s._needsPowerOn);
    // Each group of four exercises B->B, B->W, W->B, W->W respectively.
    // FAST AA should pulse both kinds of incoming black and still erase old ink.
    Bytes prior(48000, 0x33), target(48000, 0x55);
    s.display(bus, prior.data(), nullptr, RefreshMode::Full, false);
    bus.clear();
    s.beginGrayscale(bus, target.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
    unsigned baseRefreshes = 0;
    lut = nullptr;
    data = nullptr;
    for (const auto& w : bus.writes) {
      if (w.command == 0x10) data = &w.bytes;
      if (w.command == 0x12) ++baseRefreshes;
      if (w.command == 0x20) lut = &w.bytes;
    }
    assert(baseRefreshes == 1 && lut && *lut == Bytes(lut_picco_se0400_mode3, lut_picco_se0400_mode3 + 535));
    assert(data && *data == Bytes(96000, 0x9B));  // W->B top-up, B->W erase, W->B, W->W
    assert(memcmp(s._old, target.data(), target.size()) == 0);
    Bytes lsb(48000, 0x00), msb(48000, 0x00);
    lsb[0] = 0xF0;  // px 0-3 LSB
    msb[0] = 0xCC;  // px 0,1,4,5 MSB
    bus.clear();
    s.copyGrayscaleLsb(bus, lsb.data());
    s.copyGrayscaleMsb(bus, msb.data());
    const Bytes* g = nullptr;
    for (const auto& w : bus.writes)
      if (w.command == 0x10) g = &w.bytes;
    // px: (msb,lsb) = 11 11 01 01 | 10 10 00 00
    assert(g && g->size() == 96000 && (*g)[0] == 0xF5 && (*g)[1] == 0xA0);
    s.displayGray(bus, newFb.data(), false, nullptr, false);
    for (const auto& w : bus.writes)
      if (w.command == 0x20) lut = &w.bytes;
    assert(*lut == Bytes(lut_picco_se0400_gray, lut_picco_se0400_gray + 535));
    assert(lastRegister(bus, 0x02) == 0 && s._needsPowerOn);
    const auto old = Bytes(s._old, s._old + 48000);
    bus.clear();
    fakeDelayMs = 0;
    s.powerOffBetweenFrames(bus);
    assert(bus.writes.empty() && s._needsPowerOn && !s._needsInitialFull);
    assert(fakeDelayMs == 0 && old == Bytes(s._old, s._old + 48000));
    bus.clear();
    s.powerOffBetweenFrames(bus);
    assert(bus.writes.empty());
    s.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
    bool poweredOn = false;
    for (const auto& w : bus.writes) {
      if (w.command == 0x04) poweredOn = true;
      if (w.command == 0x12) assert(poweredOn);
      assert(w.command != 0x07);  // no deep sleep
    }
    assert(poweredOn && !s._needsPowerOn);
    Bytes grayBase(48000, 0x55), grayLsb(48000, 0x11), grayMsb(48000, 0x33);
    s.display(bus, grayBase.data(), nullptr, RefreshMode::Full, false);
    s.beginGrayscale(bus, grayBase.data(), GrayscaleMode::Overlay, RefreshMode::Fast, false);
    s.copyGrayscaleLsb(bus, grayLsb.data());
    s.copyGrayscaleMsb(bus, grayMsb.data());
    s.displayGray(bus, grayMsb.data(), false, nullptr, false);
    bus.clear();
    s.display(bus, grayBase.data(), nullptr, RefreshMode::Fast, false);
    unsigned activations = 0;
    for (const auto& w : bus.writes) {
      if (w.command == 0x10) assert(w.bytes == Bytes(96000, 0xB9));
      activations += w.command == 0x12;
    }
    assert(activations == 1 && !s._grayPending);
    s.deepSleep(bus);
    assert(lastRegister(bus, 0x07) == 0xA5);
    Ssd2677Driver noHistory;
    fakeHeapCapsFail = true;
    noHistory.begin(bus);
    fakeHeapCapsFail = false;
    noHistory.display(bus, white.data(), nullptr, RefreshMode::Full, false);
    bus.clear();
    noHistory.display(bus, black.data(), nullptr, RefreshMode::Fast, false);
    for (const auto& w : bus.writes)
      if (w.command == 0x20) assert(w.bytes == Bytes(lut_picco_se0400_full, lut_picco_se0400_full + 535));
    std::printf("Picco se0400: probe, init, packing and refresh passed\n");
    return 0;
  }
  checkController("SSD1677");

  auto& d = static_cast<Ssd1677Driver&>(ssd1677Driver());
  // Sleep latches the display pins; the probe must release them before driving RESET.
  assert(fakeHoldReleased[BoardConfig::ACTIVE.display.rst]);
  // The probe issued Read User ID; an unrecognized ID goes on to CMD 0x70.
  assert(fakePanelCommand == (strcmp(which, "unknown") ? 0x2E : 0x70));
  const uint8_t variant = BoardConfig::ACTIVE.displayControllerVariant;
  EpdBus bus;
  d.begin(bus);
  const Bytes gateOutput = (variant == PiccoYrd0426 || variant == PiccoDepg0397)
                               ? Bytes{0x01, 0x8F, 0x02} : Bytes{0xDF, 0x01, 0x02};
  bool sawGateOutput = false;
  for (const auto& write : bus.writes) {
    if (write.command == 0x01) {
      assert(write.bytes == gateOutput);
      sawGateOutput = true;
    }
  }
  assert(sawGateOutput);
  if (argc > 2 && !strcmp(argv[2], "retained")) {
    expectRetainedGrayStrip(d);
    std::printf("Picco %s: retained grayscale strip passed\n", which);
    return 0;
  }
  // First paint is the absolute FULL; then exercise each mode.
  if (!strcmp(which, "yrd") || !strcmp(which, "opm")) {
    assert(variant == (!strcmp(which, "yrd") ? 1 : 3));
    assert(lastRegister(bus, 0x3C) == (!strcmp(which, "yrd") ? 0x01 : 0x03));
    expectRefresh(d, RefreshMode::Full, 0xF7, nullptr);
    expectRefresh(d, RefreshMode::Half, 0xD7, nullptr);
    expectRefresh(d, RefreshMode::Fast, 0xCC, lut_picco_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);  // rails remain on between UI paints
    d.setReaderRefresh(true);
    expectRefresh(d, RefreshMode::Fast, 0x1F, nullptr);
    d.setReaderRefresh(false);
    expectRefresh(d, RefreshMode::Fast, 0xCC, lut_picco_fast);
    expectStockPiccoGray(d);
    expectRetainedGrayStrip(d);
  } else if (!strcmp(which, "depg")) {
    assert(variant == 2);
    // Retain the selected split-duration page-turn waveform.
    assert(d._cfg.fastLut == lut_picco_depg_fast);
    assert(d._cfg.fastLut[10] == 0x80 && d._cfg.fastLut[20] == 0x50);
    assert(d._cfg.fastLut[50] == 7 && d._cfg.fastLut[51] == 10);
    expectRefresh(d, RefreshMode::Full, 0xE7, lut_picco_depg_full);
    expectRefresh(d, RefreshMode::Half, 0xC7, lut_picco_depg_full);
    expectRefresh(d, RefreshMode::Fast, 0xCC, lut_picco_depg_fast);
    expectPiccoGray(d);
    expectRetainedGrayStrip(d);
  } else {
    assert(variant == 0);
    assert(d._cfg.fullLut == nullptr && d._cfg.fastLut == nullptr && d._cfg.grayLut == lut_grayscale_sticky);
  }
  expectIdlePower(d);
  std::printf("Picco %s: probe, config and refresh sequences passed\n", which);
  return 0;
}
