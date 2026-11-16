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

static void expectRefresh(Ssd1677Driver& d, RefreshMode mode, uint8_t ctrl2, const unsigned char* lut, bool turnOff = false) {
  EpdBus bus;
  const Bytes fb(48000, 0xAA);
  d.display(bus, fb.data(), fb.data(), mode, turnOff);
  assert(lastRegister(bus, 0x22) == ctrl2);
  if (d._cfg.piccoWaveforms) assert(d._isScreenOn == ((ctrl2 & 0x03) == 0));
  const Bytes* sent = lastLut(bus);
  if (lut) {
    assert(sent && *sent == Bytes(lut, lut + 105));
    assert(lastRegister(bus, 0x2C) == lut[109]);
  } else {
    assert(!sent);
  }
  if (d._cfg.fastLut == lut_picco_depg_custom_fast) {
    bool addressed = false;
    unsigned planes = 0;
    for (const auto& w : bus.writes) {
      if (w.command == 0x4F) addressed = true;
      if (w.command == 0x24 || w.command == 0x26) {
        assert(addressed);
        addressed = false;
        ++planes;
      }
    }
    assert(planes == 2);
  }
  assert(!d._customLutActive);
}

static void expectCustomDepg(Ssd1677Driver& d) {
  const auto* split = lut_picco_depg_custom_fast;
  assert(split[0] == 0x01 && split[10] == 0x88 && split[20] == 0x44);
  assert(split[50] == 8 && split[51] == 1 && split[52] == 9 && split[53] == 1);
  // Changing pixels retain 17 driven frames, with no drive during phase B.
  for (unsigned row : {10u, 20u}) {
    unsigned driven = 0;
    for (unsigned phase = 0; phase < 4; ++phase)
      if ((split[row] >> (6 - 2 * phase)) & 3) driven += split[50 + phase];
    assert(driven == 17 && ((split[row] >> 4) & 3) == 0);
  }
  for (unsigned i = 0; i < 105; ++i)
    if (i != 0 && i != 10 && i != 20 && (i < 50 || i > 53))
      assert(split[i] == lut_picco_depg_fast_slot1[i]);
  assert(!memcmp(lut_picco_depg_custom_fast + 105, lut_grayscale_sticky + 105, 5));
  EpdBus bus;
  Bytes prior(48000, 0x33), target(48000);
  for (size_t i = 0; i < target.size(); ++i) target[i] = static_cast<uint8_t>(i);
  for (bool reading : {false, true}) {
    for (bool dark : {false, true}) {
      d.begin(bus);
      d.setBackgroundHint(dark);
      d.setReaderRefresh(reading);
      d.display(bus, prior.data(), nullptr, RefreshMode::Full, false);
      bus.clear();
      d.display(bus, target.data(), nullptr, RefreshMode::Fast, false);
      const auto* lut = lut_picco_depg_custom_fast;
      assert(lastLut(bus) && *lastLut(bus) == Bytes(lut, lut + 105));
      assert(lastRegister(bus, 0x03) == lut[105]);
      assert(lastRegister(bus, 0x2C) == lut[109]);
      unsigned planes = 0, activations = 0;
      for (const auto& w : bus.writes) {
        if (w.command == 0x04) assert(w.bytes == Bytes(lut + 106, lut + 109));
        activations += w.command == 0x20;
        if (w.command != 0x24 && w.command != 0x26) continue;
        ++planes;
        Bytes expected = w.command == 0x24 ? target : prior;
        if (w.command == 0x26 && dark) {
          expected = target;
          for (auto& b : expected) b = static_cast<uint8_t>(~b);
        }
        assert(w.bytes == expected);
      }
      assert(planes == 2 && activations == 1);
      assert(Bytes(d._piccoOld, d._piccoOld + 48000) == target);
      bus.clear();
      d.display(bus, target.data(), nullptr, RefreshMode::Half, false);
      assert(lastLut(bus) && *lastLut(bus) == Bytes(lut_picco_depg_full, lut_picco_depg_full + 105));
      for (const auto& w : bus.writes)
        if (w.command == 0x24 || w.command == 0x26) assert(w.bytes == target);
    }
  }
  d.setBackgroundHint(false);
  d.setReaderRefresh(false);
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
        assert(w.bytes[i] == static_cast<uint8_t>(~expected));
      }
    }
    // Factory gray powers down separately after its single image activation.
    assert(planes == 2 && activations == 2u);
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
    // Absolute planes use the X4 factory encoding (black=11), so they are complemented.
    for (const auto& w : bus.writes) {
      Bytes expected = w.command == 0x24 ? lsb : msb;
      if (absolute) for (auto& b : expected) b = static_cast<uint8_t>(~b);
      if (w.command == 0x24 || w.command == 0x26) assert(w.bytes == expected);
    }
    bus.clear();
    const bool wasOn = d._isScreenOn;
    d.displayGray(bus, msb.data(), absolute, nullptr, absolute);
    if (absolute) {
      // Images carry their own voltages: the X4 factory LUT, not a stock 105-byte
      // table that would inherit the last B/W LUT's VCOM/VSH2.
      assert(lastLut(bus) && *lastLut(bus) == Bytes(lut_factory_quality, lut_factory_quality + 105));
      assert(lastRegister(bus, 0x2C) == lut_factory_quality[109]);
      for (const auto& w : bus.writes)
        if (w.command == 0x04) assert(w.bytes == Bytes(lut_factory_quality + 106, lut_factory_quality + 109));
      // 0xCC paint, then a separate 0x03 shutdown.
      assert(lastRegister(bus, 0x22) == 0x03);
      assert(d._needsGrayClear && !d._isScreenOn);
      continue;
    }
    const auto* lut = d._cfg.grayLut;
    if (lut) {
      Bytes expected(lut, lut + 105);
      if (!absolute && d._cfg.fastLut == lut_picco_depg_custom_fast)
        for (unsigned index : {12u, 22u, 32u}) expected[index] |= 0x01;
      assert(lastLut(bus) && *lastLut(bus) == expected);
    } else assert(!lastLut(bus));
    const bool stickyAa = !absolute && lut == lut_grayscale_sticky;
    uint8_t sequence = lut ? 0xC7 : 0xD7;
    if (wasOn) sequence &= 0x3F;
    // Stock follows the caller: retain power for AA here, shut down for the image.
    if (!absolute) sequence &= 0xFC;
    if (stickyAa) sequence = 0xCC;
    assert(lastRegister(bus, 0x22) == sequence);
    if (stickyAa) {
      assert(lastRegister(bus, 0x03) == lut[105]);
      assert(lastRegister(bus, 0x2C) == lut[109]);
      assert(lastRegister(bus, 0x3C) == 0x80);
      for (const auto& w : bus.writes)
        if (w.command == 0x04) assert(w.bytes == Bytes(lut + 106, lut + 109));
    }
    unsigned activations = 0;
    for (const auto& w : bus.writes) {
      if (!stickyAa) assert(w.command != 0x03 && w.command != 0x04 && w.command != 0x2C);
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

static void expectStickyAaPower(Ssd1677Driver& d) {
  EpdBus bus;
  Bytes base(48000, 0x55), lsb(48000, 0x11), msb(48000, 0x33);
  for (bool turnOff : {false, true}) {
    d.begin(bus);
    d.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Full, true);
    d.copyGrayscaleLsb(bus, lsb.data());
    d.copyGrayscaleMsb(bus, msb.data());
    assert(!d._isScreenOn);
    bus.clear();
    d.displayGray(bus, msb.data(), turnOff, nullptr, false);
    Bytes expected(lut_grayscale_sticky, lut_grayscale_sticky + 105);
    expected[12] = 0x41;
    expected[22] = 0xA9;
    expected[32] = 0x21;
    assert(lastLut(bus) && *lastLut(bus) == expected);
    assert(lastRegister(bus, 0x2C) == lut_grayscale_sticky[109]);
    std::vector<uint8_t> sequences;
    for (const auto& w : bus.writes) {
      if (w.command == 0x22) sequences.push_back(w.bytes[0]);
      assert(w.command != 0x1A);  // Sticky AA does not load a fixed temperature.
    }
    assert(sequences == std::vector<uint8_t>({0xC0, static_cast<uint8_t>(turnOff ? 0xCF : 0xCC)}));
    assert(d._isScreenOn == !turnOff && !d._customLutActive);
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
  assert(lastRegister(bus, 0x22) == (d._cfg.piccoWaveforms ? (startedSequence | 0xC0) : startedSequence));
  d.displayFinish(bus, fb.data());
  // Reader AA powers off; RAM cleanup and repeated idle shutdown remain safe.
  d.displayGray(bus, fb.data(), false, nullptr, false);
  assert(d._isScreenOn);
  d.cleanupGrayscaleBuffers(bus, fb.data());
  frame.powerOffBetweenFrames();
  assert(!d._isScreenOn && !d._inGrayscaleMode && !d._needsInitialFull);
  d.displayGray(bus, fb.data(), false, nullptr, true);
  frame.powerOffBetweenFrames();
  assert(!d._isScreenOn && d._needsGrayClear);
}

static void expectSsd2677NightMode(Ssd2677Driver& driver) {
  for (bool reading : {false, true}) {
    for (auto mode : {FreeInkDisplay::FAST_REFRESH, FreeInkDisplay::HALF_REFRESH, FreeInkDisplay::FULL_REFRESH}) {
      FreeInkDisplay display(1, 2, 3, 4, 5, 6);
      display._driver = &driver;
      display.begin();
      driver.setReaderRefresh(reading);
      display.setInverted(true);
      std::memset(display.getFrameBuffer(), 0xCC, 48000);
      display.displayBuffer(FreeInkDisplay::FULL_REFRESH, true);
      std::memset(display.getFrameBuffer(), 0xAA, 48000);
      display._bus.clear();
      display.displayBuffer(mode, true);
      const bool fast = mode == FreeInkDisplay::FAST_REFRESH;
      const unsigned expectedFrames = 1;
      // A whole-screen change is a redraw: mode 2 in both contexts.
      const auto* lut = fast ? lut_picco_se0400_fast : lut_picco_se0400_full;
      unsigned frames = 0, activations = 0, lutLoads = 0;
      for (const auto& w : display._bus.writes) {
        if (w.command == 0x10) {
          assert(activations == frames);
          assert(w.bytes == Bytes(96000, fast ? 0x9B : 0x1B));
          ++frames;
        }
        if (w.command == 0x20) {
          assert(w.bytes == Bytes(lut, lut + 535));
          ++lutLoads;
        }
        if (w.command == 0x12) ++activations;
      }
      assert(frames == expectedFrames && activations == expectedFrames && lutLoads == expectedFrames);
      assert(Bytes(driver._old, driver._old + 48000) == Bytes(48000, 0x55));
      assert(driver._needsPowerOn);
      display.setInverted(false);
    }
  }
  driver.setReaderRefresh(false);
}

// BOOX 2959 overlays reader AA on inverted pages: masks keep their encoding,
// the retained B/W baseline keeps wire polarity, and the next page drives gray pixels.
static void expectNightGray(Ssd1677Driver& d) {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &d;
  display.begin();
  d.setReaderRefresh(true);
  display.setInverted(true);
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayBuffer(FreeInkDisplay::HALF_REFRESH, false);  // consumes the inversion clean
  if (!d._cfg.piccoWaveforms || d._cfg.fastLut == lut_picco_depg_custom_fast) {
    assert(!display.grayscaleCapabilities().supported());
    display._bus.clear();
    assert(!display.displayGrayscaleBase(GrayscaleMode::Overlay, FreeInkDisplay::FAST_REFRESH, false));
    const Bytes mask(48000, 0x55);
    display.copyGrayscaleLsbBuffers(mask.data());
    display.copyGrayscaleMsbBuffers(mask.data());
    display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, mask.data(), 0, 480);
    display.displayGrayBuffer(false);
    assert(display._bus.writes.empty());
    display.setInverted(false);
    if (d._cfg.fastLut == lut_picco_depg_custom_fast)
      assert(display.grayscaleCapabilities().supported());
    d.setReaderRefresh(false);
    return;
  }
  assert(display.grayscaleCapabilities().supported());
  const Bytes lsb(48000, 0x00), msb(48000, 0x03);
  uint8_t* fb = display.getFrameBuffer();
  std::memset(fb, 0xF0, 48000);
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
  display._bus.clear();
  display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 0, 480);
  display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_MSB, msb.data(), 0, 480);
  display.displayGrayBuffer(false);
  bool lsbSeen = false, msbSeen = false;
  for (const auto& w : display._bus.writes) {
    if (w.command == 0x24 && w.bytes.size() == 48000) lsbSeen = w.bytes == lsb;
    if (w.command == 0x26 && w.bytes.size() == 48000) msbSeen = w.bytes == msb;
  }
  assert(lsbSeen && msbSeen && lastRegister(display._bus, 0x22) != 0);
  display.cleanupGrayscaleBuffers(fb);
  assert(fb[0] == 0xF0 && d._piccoOld[0] == 0x0F);
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
  const Bytes* old = nullptr;
  for (const auto& w : display._bus.writes)
    if (w.command == 0x26) old = &w.bytes;
  // (0x0F & ~0x03) | (~0x00 & 0x03): gray pixels start opposite the new target.
  assert(old && (*old)[0] == (d._cfg.fastLut == lut_picco_depg_custom_fast ? 0xFF : 0x0F));
  display.setInverted(false);
  d.setReaderRefresh(false);
}

// BOOX 2959 overlays reader AA on inverted SE0400 pages (MSB high, LSB low);
// the next page starts masked pixels opposite their target.
static void expectNightGray2677(Ssd2677Driver& s) {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &s;
  display.begin();
  s.setReaderRefresh(true);
  display.setInverted(true);
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayBuffer(FreeInkDisplay::HALF_REFRESH, false);
  assert(display.grayscaleCapabilities().supported());
  std::memset(display.getFrameBuffer(), 0xFF, 48000);
  display.displayGrayscaleBase(FreeInkDisplay::FAST_REFRESH, false);
  const Bytes lsb(48000, 0x00), msb(48000, 0xFF);
  display._bus.clear();
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.copyGrayscaleMsbBuffers(msb.data());
  display.displayGrayBuffer(false);
  const Bytes* gray = nullptr;
  for (const auto& w : display._bus.writes)
    if (w.command == 0x10) gray = &w.bytes;
  assert(gray && *gray == Bytes(96000, 0xAA) && s._grayPending);  // (msb 1, lsb 0) per pixel
  std::memset(display.getFrameBuffer(), 0x00, 48000);  // logical black = wire white
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
  const Bytes* page = nullptr;
  for (const auto& w : display._bus.writes)
    if (w.command == 0x10) page = &w.bytes;
  assert(page && *page == Bytes(96000, 0x55));  // old 0 (opposite), new 1
  display.setInverted(false);
  s.setReaderRefresh(false);
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
    // First paint is forced FULL, drawn from the white seed in one pass.
    const Bytes* data = nullptr;
    unsigned frames = 0;
    for (const auto& w : bus.writes)
      if (w.command == 0x10) {
        ++frames;
        data = &w.bytes;
      }
    assert(frames == 1 && data && data->size() == 96000);
    assert((*data)[0] == 0xAA && (*data)[1] == 0xFF);  // white->black, white->white
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
    // Reader mode retains unchanged pixels without the UI black top-up.
    assert(data && data->size() == 96000 && (*data)[0] == 0x00 && (*data)[1] == 0xFF);
    // Reader refreshes power down even when the host requests retention.
    bus.clear();
    s.display(bus, newFb.data(), nullptr, RefreshMode::Fast, false);
    assert(lastRegister(bus, 0x02) == 0 && s._needsPowerOn);
    s.setReaderRefresh(false);
    assert(lastRegister(bus, 0x02) == 0x00);
    for (unsigned uiFrame = 0; uiFrame < 2; ++uiFrame) {
      bus.clear();
      s.display(bus, newFb.data(), nullptr, RefreshMode::Fast, true);
      data = nullptr;
      lut = nullptr;
      for (const auto& w : bus.writes) {
        if (w.command == 0x10) data = &w.bytes;
        if (w.command == 0x20) lut = &w.bytes;
      }
      // The first frame after a book page still uses the reading waveform.
      const auto* expected = uiFrame == 0 ? lut_picco_se0400_fast : lut_picco_se0400_mode3;
      assert(lut && *lut == Bytes(expected, expected + 535));
      assert(data && data->size() == 96000 && (*data)[1] == 0xFF);
      assert(s._needsPowerOn == (uiFrame == 0));
    }

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
    assert(s._needsPowerOn);  // a whole-screen UI change is a mode 2 redraw and powers off
    {
      // Light UI redraw tops up only bytes that changed; static black text is left alone.
      Bytes half(48000, 0x00);
      std::fill(half.begin() + 24000, half.end(), 0xFF);
      s.display(bus, black.data(), nullptr, RefreshMode::Full, false);
      bus.clear();
      s.display(bus, half.data(), nullptr, RefreshMode::Fast, false);
      const Bytes* frame = nullptr;
      for (const auto& w : bus.writes)
        if (w.command == 0x10) frame = &w.bytes;
      assert(frame && (*frame)[0] == 0x00 && (*frame)[48000] == 0x55);
    }
    // Overlay AA: LSB mask -> low bit, MSB mask -> high bit, slot-5 LUT.
    assert(s.grayscaleCapabilities(GrayscaleMode::Overlay).supported());
    assert(s.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
    // Black, dark, light, white, repeated across the complete slot-4 frame.
    Bytes imageLsb(48000, 0x55), imageMsb(48000, 0x33);
    bus.clear();
    s.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, RefreshMode::Half, false);
    s.copyGrayscaleLsb(bus, imageLsb.data());
    s.copyGrayscaleMsb(bus, imageMsb.data());
    assert(bus.writes.empty());
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
    assert((imageCommands == std::vector<uint8_t>{0x04, 0x10, 0x20, 0x12, 0x02}));
    assert(s._needsInitialFull && s._needsPowerOn);
    // Each group of four exercises B->B, B->W, W->B, W->W respectively.
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
    assert(baseRefreshes == 1 && lut && *lut == Bytes(lut_picco_se0400_fast, lut_picco_se0400_fast + 535));
    assert(data && *data == Bytes(96000, 0x9B));  // B->B top-up, B->W, W->B, W->W
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
    // px: both masks and LSB-only go out as 01, MSB-only as 10: 01 01 01 01 | 10 10 00 00
    assert(g && g->size() == 96000 && (*g)[0] == 0x55 && (*g)[1] == 0xA0);
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
    assert(poweredOn && s._needsPowerOn);  // whole-screen redraw powers back off
    expectSsd2677NightMode(s);
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
    expectNightGray2677(s);
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
  if (variant == PiccoDepg0397) {
    const std::vector<uint8_t> commands{0x12, 0x01, 0x11, 0x44, 0x45, 0x4E, 0x4F, 0x3C, 0x21, 0x0C, 0x18};
    const std::vector<Bytes> data{{}, {1, 0x8F, 2}, {1}, {0, 0, 3, 0x1F}, {1, 0x8F, 0, 0},
                                 {0, 0}, {1, 0x8F}, {1}, {0, 0}, {0xAE, 0xC7, 0xC3, 0xC0, 0x80}, {0x80}};
    assert(bus.writes.size() == commands.size());
    for (unsigned i = 0; i < commands.size(); ++i) {
      assert(bus.writes[i].command == commands[i]);
      assert(bus.writes[i].bytes == data[i]);
    }
  }
  if (argc > 2 && !strcmp(argv[2], "retained")) {
    expectRetainedGrayStrip(d);
    std::printf("Picco %s: retained grayscale strip passed\n", which);
    return 0;
  }
  // First paint is the absolute FULL; then exercise each mode.
  if (!strcmp(which, "yrd") || !strcmp(which, "opm")) {
    assert(variant == (!strcmp(which, "yrd") ? 1 : 3));
    assert(lastRegister(bus, 0x3C) == (!strcmp(which, "yrd") ? 0x01 : 0x80));
    expectRefresh(d, RefreshMode::Full, 0xF4, nullptr);
    expectRefresh(d, RefreshMode::Half, 0x14, nullptr);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);  // rails remain on between UI paints
    d.setReaderRefresh(true);
    expectRefresh(d, RefreshMode::Fast, 0x1C, nullptr);
    d.setReaderRefresh(false);
    // First UI frame retains the reader waveform to erase reading-depth ink.
    expectRefresh(d, RefreshMode::Fast, 0x1C, nullptr);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_fast);
    expectStockPiccoGray(d);
    expectRetainedGrayStrip(d);
  } else if (!strcmp(which, "depg")) {
    assert(variant == 2);
    // UI and reader share the custom pulse timing and voltage calibration.
    assert(d._cfg.fastLut == lut_picco_depg_custom_fast);
    assert(d._cfg.fastLut[10] == 0x88 && d._cfg.fastLut[20] == 0x44);
    assert(d._cfg.fastLut[50] == 8 && d._cfg.fastLut[51] == 1);
    expectRefresh(d, RefreshMode::Full, 0xE4, lut_picco_depg_full);
    expectRefresh(d, RefreshMode::Half, 0x04, lut_picco_depg_full);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    d.setReaderRefresh(true);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0F, lut_picco_depg_custom_fast, true);
    expectRefresh(d, RefreshMode::Fast, 0xCC, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    d.setReaderRefresh(false);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectRefresh(d, RefreshMode::Fast, 0x0C, lut_picco_depg_custom_fast);
    expectStockPiccoGray(d);
    expectRetainedGrayStrip(d);
  } else {
    assert(variant == 0);
    assert(d._cfg.fullLut == nullptr && d._cfg.fastLut == nullptr && d._cfg.grayLut == lut_grayscale_sticky);
  }
  if (variant == PiccoDepg0397) { expectCustomDepg(d); expectStickyAaPower(d); }
  expectNightGray(d);
  expectIdlePower(d);
  std::printf("Picco %s: probe, config and refresh sequences passed\n", which);
  return 0;
}
