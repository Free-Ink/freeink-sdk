#include <cassert>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstring>
#include <type_traits>
// Inspect private state without adding test-only methods to the SDK API.
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/driver/Uc8179Driver.h"
#include "src/driver/Uc8279X4Driver.h"
#undef private

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static uint8_t lastRegister(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command && !i->bytes.empty()) return i->bytes[0];
  assert(false);
  return 0;
}

static Bytes frame(unsigned seed) {
  Bytes b(48000);
  for (size_t i=0; i<b.size(); ++i) b[i]=uint8_t((i*37 + i/100*11 + seed) ^ (i>>8));
  return b;
}

template<class Driver>
static void testStream(bool reverse, unsigned gateOffset) {
  Driver d;
  EpdBus bus;
  const auto a=frame(3), b=frame(89);
  for (bool invert : {false,true}) {
    bus.clear();
    d.streamPlane(bus,0x13,a.data(),invert);
    assert(bus.writes.size()==1);
    const auto& w=bus.writes.front();
    assert(w.transactions==1 && w.bytes.size()==60000);
    for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
      uint8_t expected=0xff;
      if (y>=gateOffset && y<gateOffset+480) {
        unsigned row=y-gateOffset;
        if (reverse) row=479-row;
        expected=a[row*100+x];
        if (invert) expected=uint8_t(~expected);
      }
      assert(w.bytes[y*100+x]==expected);
    }
  }
  bus.clear();
  d.streamPlaneXor(bus,0x13,a.data(),b.data());
  assert(bus.writes.size()==1 && bus.writes[0].transactions==1);
  assert(bus.writes[0].bytes.size()==60000);
  for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
    uint8_t expected=0xff;
    if (y>=gateOffset && y<gateOffset+480) {
      unsigned row=y-gateOffset;
      if (reverse) row=479-row;
      expected=a[row*100+x]^b[row*100+x];
    }
    assert(bus.writes[0].bytes[y*100+x]==expected);
  }
  d._grayRefreshedOnce=true;
  d._oldPlaneValid=true;
  d._needFullClear=false;
  bus.clear();
  d.displayGrayscaleBase(bus,a.data(),RefreshMode::Full,false);
  assert(lastRegister(bus,0xe5)==0x1e); // honor Full even after an AA page
  for (const auto& w : bus.writes)
    if (w.command==0x10 || w.command==0x13) assert(w.transactions==1);
}

static void testSsd() {
  const auto b=frame(33);
  for (auto mode : {RefreshMode::Full,RefreshMode::Half,RefreshMode::Fast}) {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    bus.clear();
    d.display(bus,b.data(),nullptr,mode,false);
    assert(lastRegister(bus,0x22)==(mode==RefreshMode::Full ? 0xf7 : 0xd7));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,false);
    assert(lastRegister(bus,0x22)==0xfc);
  }
  // The first-paint-after-boot/wake promotion must survive turnOff. A caller that powers
  // the panel down on every refresh (CrossPoint's sunlight fading fix) otherwise never
  // consumes the one-shot, and its differential FAST cannot clear the sleep screen.
  {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    // The activation value is not the LAST 0x22 write here: 0xfc does not self-power-off,
    // so the driver follows it with the separate 0x22=0x03 power-down sequence.
    const auto activated = [&bus](uint8_t seq) {
      for (const auto& w : bus.writes)
        if (w.command == 0x22 && !w.bytes.empty() && w.bytes[0] == seq) return true;
      return false;
    };
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,true);
    assert(activated(0xd7) && !activated(0xfc));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,true);
    assert(activated(0xfc) && !activated(0xd7));
  }
  EpdBus bus;
  Ssd1677Driver d;
  d.begin(bus);
  bus.clear();
  d.displayGray(bus,b.data(),false,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcc && d._isScreenOn);
  d.deepSleep(bus);
  assert(lastRegister(bus,0x22)==3 && !d._isScreenOn);
  bus.clear();
  d.displayGray(bus,b.data(),true,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcf && !d._isScreenOn);
}

template<class Driver>
static void testAsyncFrame() {
  Driver driver;
  FreeInkDisplay display(12,11,13,18,14,6);
  display._driver=&driver;
  display.begin();
  const auto submitted=frame(27), redrawn=frame(94);
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending());
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.completeDisplay();
  // Finish must sync the frame actually submitted, even after the caller draws.
  const auto& old=display._bus.writes.back();
  assert(old.command==0x10 && old.bytes.size()==60000);
  for (unsigned y=0; y<480; ++y) {
    unsigned dst;
    if constexpr (std::is_same<Driver,Uc8179Driver>::value) dst=479-y;
    else dst=120+y;
    assert(std::equal(submitted.begin()+y*100,submitted.begin()+(y+1)*100,old.bytes.begin()+dst*100));
  }
  assert(!display.isRefreshPending());
  // Shadow-free entry points require the live frame to survive until finish.
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH,false);
  display.completeDisplay();
  const auto& next=display._bus.writes.back();
  unsigned firstRow=std::is_same<Driver,Uc8179Driver>::value ? 479 : 0;
  unsigned offset=std::is_same<Driver,Uc8179Driver>::value ? 0 : 12000;
  assert(std::equal(redrawn.begin()+firstRow*100,redrawn.begin()+(firstRow+1)*100,next.bytes.begin()+offset));
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayAsyncImpl(FreeInkDisplay::FAST_REFRESH,false,true);
  display.completeDisplay();
  const auto& noShadow=display._bus.writes.back();
  assert(std::equal(submitted.begin()+firstRow*100,submitted.begin()+(firstRow+1)*100,
                    noShadow.bytes.begin()+offset));
#endif
  display.releaseBuffers();
  free(driver._grayBase);
}

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
// cleanupGrayscaleWithPreviousBuffer() only ever runs at the tail (or abort point) of a
// grayscale plane pass, and those passes use the write framebuffer as plane scratch. So
// frameBufferActive is the only thing that can be the displayed B/W frame: with the secondary
// away the driver must be told there is no baseline, never handed the plane. Handing it over
// rebases the controller's previous-frame RAM from the page's own glyph plane.
static void testGrayCleanupBaseline() {
  const auto page = frame(51), planeScratch = frame(77);

  // Secondary resident: the displayed frame is the baseline, and the write buffer is restored
  // from it so the next B/W draw starts from a real frame rather than plane bytes.
  {
    Uc8279X4Driver driver;
    FreeInkDisplay display(12, 11, 13, 18, 14, 6);
    display._driver = &driver;
    display.begin();
    std::memcpy(display.getFrameBuffer(), page.data(), page.size());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);  // swaps: frameBufferActive == page
    std::memcpy(display.getFrameBuffer(), planeScratch.data(), planeScratch.size());
    display._bus.clear();
    display.cleanupGrayscaleWithPreviousBuffer();
    assert(std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x10; }) == 1);
    assert(driver._oldPlaneValid);
    assert(std::equal(page.begin(), page.end(), display.getFrameBuffer()));
    display.releaseBuffers();
    free(driver._grayBase);
  }

  // Secondary released, then lent: in both cases no copy of the displayed frame exists, so
  // nothing may be streamed into controller RAM and the driver must fall back to a clean sync.
  for (bool lend : {false, true}) {
    Uc8279X4Driver driver;
    FreeInkDisplay display(12, 11, 13, 18, 14, 6);
    display._driver = &driver;
    display.begin();
    std::memcpy(display.getFrameBuffer(), page.data(), page.size());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(driver._oldPlaneValid);
    if (lend) assert(display.borrowSecondaryBuffer(nullptr) != nullptr);
    else assert(display.releaseSecondaryBuffer());
    std::memcpy(display.getFrameBuffer(), planeScratch.data(), planeScratch.size());
    display._bus.clear();
    display.cleanupGrayscaleWithPreviousBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x10 && w.command != 0x13);
    assert(driver._needFullClear && !driver._oldPlaneValid);
    if (lend) assert(display.returnSecondaryBuffer());
    display.releaseBuffers();
    free(driver._grayBase);
  }
}
#endif

// A driver with no grayscale implementation must never advertise support.
class BwOnlyDriver : public PanelDriver {
 public:
  uint32_t spiHz() const override { return 10000000; }
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }
  PanelGeometry geometry() const override { return {800, 480, 100, 48000}; }
  void begin(EpdBus&) override {}
  void deepSleep(EpdBus&) override {}
  void display(EpdBus&, const uint8_t*, const uint8_t*, RefreshMode, bool) override {}
};

class CombinedGrayDriver : public BwOnlyDriver {
 public:
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, true, false, true};
  }
};

static void testCapabilities() {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  assert(!display.grayscaleCapabilities().supported());
  BwOnlyDriver bw;
  display._driver = &bw;
  assert(!display.grayscaleCapabilities().supported());
  assert(!display.supportsStripGrayscale());

  Ssd1677Driver ssd;
  display._driver = &ssd;
  auto caps = display.grayscaleCapabilities();
  assert(caps.supported() && caps.encoding == GrayscaleEncoding::OverlayMasks);
  assert(caps.stripUploads && caps.asyncBase && !caps.stagingWhileBusy);
  assert(caps.base == GrayscaleBase::Separate);
  assert(display.supportsStripGrayscale() == caps.stripUploads);
  assert(display.supportsAsyncGrayscaleBase() == caps.asyncBase);
  assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(!display.grayscaleCapabilities(static_cast<GrayscaleMode>(255)).supported());
  display._inversionDirty = true;
  assert(!display.grayscaleCapabilities().asyncBase);
  assert(display.grayscaleCapabilities().stripUploads);
  assert(!display.supportsAsyncGrayscaleBase());
  display._inversionDirty = false;

  CombinedGrayDriver combined;
  display._driver = &combined;
  assert(display.combinesGrayscaleBase());
  assert(display.supportsBusyGrayscaleStaging());
  assert(!display.supportsAsyncGrayscaleBase());
  display._inverted = true;
  caps = display.grayscaleCapabilities();
  assert(!caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
  assert(!display.combinesGrayscaleBase() && !display.supportsBusyGrayscaleStaging());
  assert(!display.supportsStripGrayscale());
  display._inverted = false;

  Uc8179Driver uc8179;
  Uc8279X4Driver uc8279;
  for (PanelDriver* driver : {static_cast<PanelDriver*>(&uc8179), static_cast<PanelDriver*>(&uc8279)}) {
    display._driver = driver;
    caps = display.grayscaleCapabilities();
    assert(caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
    assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
    assert(!display.grayscaleCapabilities(GrayscaleMode::Absolute).stripUploads);
  }
  // Queries must neither start a refresh nor write the panel bus.
  assert(display._bus.writes.empty() && !display.isRefreshPending());
}

static void testAbsolutePipeline() {
  Ssd1677Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto absolute = GrayscaleMode::Absolute;
  assert(display.grayscaleCapabilities(absolute).base == GrayscaleBase::Combined);
  for (bool strips : {false, true}) {
    display._bus.clear();
    assert(display.displayGrayscaleBase(absolute));
    assert(display._bus.writes.empty());
    if (strips) {
      for (unsigned y = 0; y < 480; y += 80) {
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data() + y * 100, y, 80);
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_MSB, msb.data() + y * 100, y, 80);
      }
    } else display.copyGrayscaleBuffers(lsb.data(), msb.data());
    Bytes plane0, plane1;
    for (const auto& w : display._bus.writes) {
      if (w.command == 0x24) plane0.insert(plane0.end(), w.bytes.begin(), w.bytes.end());
      if (w.command == 0x26) plane1.insert(plane1.end(), w.bytes.begin(), w.bytes.end());
    }
    assert(plane0.size() == lsb.size() && plane1.size() == msb.size());
    for (size_t i = 0; i < lsb.size(); ++i) {
      assert(plane0[i] == uint8_t(~lsb[i]) && plane1[i] == uint8_t(~msb[i]));
    }
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.displayGrayBuffer(false);
    assert(std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x20; }) == 1);
    assert(lastRegister(display._bus, 0x22) == 0xCC);
    assert(driver._isScreenOn && driver._needsGrayClear);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needsGrayClear);
    display._bus.clear();
    display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
    display.waitRefreshComplete();
    assert(!driver._needsGrayClear);
  }
  // Explicit shutdown must use its own activation after the gray refresh wait.
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display._bus.clear();
  display.displayGrayBuffer(true);
  std::vector<uint8_t> activations;
  for (const auto& w : display._bus.writes) {
    if (w.command == 0x22) activations.push_back(w.bytes.at(0));
  }
  assert((activations == std::vector<uint8_t>{0xCC, 0x03}));
  assert(!driver._isScreenOn && driver._needsGrayClear);
  for (unsigned failure = 0; failure < 6; ++failure) {
    assert(display.displayGrayscaleBase(absolute));
    display._bus.clear();
    if (failure == 0) display.copyGrayscaleLsbBuffers(lsb.data()); // missing MSB
    if (failure == 1) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 80, 80);
    if (failure == 2) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 0, 481);
    if (failure == 3) display.copyGrayscaleLsbBuffers(nullptr);
    if (failure == 4) {
      display.copyGrayscaleBuffers(lsb.data(), msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data()); // duplicate plane
    }
    if (failure == 5) {
      display.copyGrayscaleMsbBuffers(msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data());
    }
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.cleanupGrayscaleBuffers(nullptr);
    display._bus.clear();
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, true);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
  }
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.setInverted(true);
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  assert(!display.displayGrayscaleBase(absolute));
  display.setInverted(false);
  display._bus.clear();
  assert(display.displayGrayscaleBase(absolute));
  assert(display._bus.writes.empty() && display._inversionDirty);
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer();
  assert(!display._inversionDirty);
  assert(display.displayGrayscaleBase(absolute));
  display.deepSleep();
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  display.releaseBuffers();

  Ssd1677Config unsupported = ssd1677DefaultConfig();
  unsupported.absoluteGrayscale = false;
  Ssd1677Driver other(unsupported);
  FreeInkDisplay noAbsolute(1, 2, 3, 4, 5, 6);
  noAbsolute._driver = &other;
  assert(!noAbsolute.displayGrayscaleBase(absolute));
  assert(noAbsolute._bus.writes.empty());
}

template<class Driver>
static void testUltraChipAbsolute(bool reverse, unsigned gateOffset, bool inverted) {
  for (bool snapshot : {false, true}) for (bool turnOff : {false, true}) {
    Driver driver;
    FreeInkDisplay display(1, 2, 3, 4, 5, 6);
    display._driver = &driver;
    display.begin();
    if (!snapshot) { free(driver._grayBase); driver._grayBase = nullptr; }
    const auto bw = frame(91), lsb = frame(13), msb = frame(29);
    memcpy(display.getFrameBuffer(), bw.data(), bw.size());
    assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
    display._bus.clear();
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    for (unsigned p = 0; p < 2; ++p) {
      const auto& input = p ? msb : lsb;
      const auto command = p ? 0x13 : 0x10;
      const auto it = std::find_if(display._bus.writes.begin(), display._bus.writes.end(),
                                  [command](const auto& w) { return w.command == command; });
      assert(it != display._bus.writes.end() && it->bytes.size() == 60000);
      for (unsigned y = 0; y < 600; ++y) for (unsigned x = 0; x < 100; ++x) {
        uint8_t expected = 0xff;
        if (y >= gateOffset && y < gateOffset + 480) {
          unsigned row = y - gateOffset;
          if (reverse) row = 479 - row;
          expected = input[row * 100 + x];
          if (inverted) expected = uint8_t(~expected);
        }
        assert(it->bytes[y * 100 + x] == expected);
      }
    }
    if (snapshot) assert(memcmp(driver._grayBase, bw.data(), bw.size()) == 0);
    display.displayGrayBuffer(turnOff);
    assert(driver._isScreenOn == !turnOff);
    assert(driver._needFullClear && !driver._absoluteInput);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needFullClear);
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(!driver._needFullClear);
    // Incomplete uploads are canceled without a gray activation and still force a clean.
    assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
    display.copyGrayscaleLsbBuffers(lsb.data());
    display._bus.clear();
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x12);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needFullClear && !driver._absoluteInput);
    display.displayGrayscaleBase(FreeInkDisplay::HALF_REFRESH);
    assert(!driver._absoluteInput);
    display.releaseBuffers();
    free(driver._grayBase);
    driver._grayBase = nullptr;
  }
}

static void testUc8179GrayShadeSplit() {
  Uc8179Driver driver;
  freeink::EpdBus bus;
  driver.begin(bus);
  const auto bw = frame(91), lsb = frame(13), msb = frame(29);
  for (auto mode : {GrayscaleMode::Absolute, GrayscaleMode::Overlay}) {
    driver.beginGrayscale(bus, bw.data(), mode, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
    driver.displayGray(bus, bw.data(), false, nullptr, mode == GrayscaleMode::Absolute);
    const auto light = std::find_if(bus.writes.begin(), bus.writes.end(),
                                    [](const auto& w) { return w.command == 0x22; });
    const auto dark = std::find_if(bus.writes.begin(), bus.writes.end(),
                                   [](const auto& w) { return w.command == 0x23; });
    assert(light != bus.writes.end() && dark != bus.writes.end());
    assert(light->bytes.size() == 42 && dark->bytes.size() == 42);
    assert(light->bytes[0] == 0x20 && light->bytes[1] == 2 && light->bytes[2] == 2);
    auto expected = light->bytes;
    expected[2] = 1;
    expected[3] = 2;
    assert(dark->bytes == expected);
    std::vector<size_t> pllWrites;
    size_t refresh = bus.writes.size();
    for (size_t i = 0; i < bus.writes.size(); ++i) {
      if (bus.writes[i].command == 0x30 && bus.writes[i].bytes.size() == 1) pllWrites.push_back(i);
      if (bus.writes[i].command == 0x12) refresh = i;
    }
    if (mode == GrayscaleMode::Absolute) {
      assert(pllWrites.size() == 2);
      assert(bus.writes[pllWrites[0]].bytes[0] == 0x05 && bus.writes[pllWrites[1]].bytes[0] == 0x06);
      assert(pllWrites[0] < refresh && refresh < pllWrites[1]);
    } else {
      assert(pllWrites.empty());
    }
  }
  free(driver._grayBase);
}

template<class Driver>
static void testDirectSleep() {
  Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(11), lsb = frame(17), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto activations = [&]() {
    return std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x12; });
  };
  assert(display.grayscaleCapabilities(GrayscaleMode::Direct).base == GrayscaleBase::Combined);
  for (auto fallback : {FreeInkDisplay::HALF_REFRESH, FreeInkDisplay::FAST_REFRESH}) {
    display._bus.clear();
    assert(display.displayGrayscaleBase(GrayscaleMode::Direct, fallback));
    assert(activations() == 0);
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    assert(activations() == 0);
    display.displayGrayBuffer(false);
    assert(activations() == 1);
    display.cleanupGrayscaleBuffers(bw.data());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
    assert(activations() > 1);
  }
  display._bus.clear();
  assert(display.displayGrayscaleBase(GrayscaleMode::Direct));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.displayGrayBuffer(false);
  assert(activations() == 0);
  display.deepSleep();
  free(driver._grayBase);
}

static void testUc8279X4WaveformSelection() {
  Uc8279X4Driver driver;
  EpdBus bus;
  driver.begin(bus);
  const auto bw = frame(11);
  Bytes lsb(48000, 0), msb(48000, 0);
  const auto bank = [&]() {
    std::vector<Bytes> rows(5);
    for (const auto& w : bus.writes)
      if (w.command >= 0x20 && w.command <= 0x24 && w.bytes.size() == 49) rows[w.command - 0x20] = w.bytes;
    for (const auto& row : rows) assert(row.size() == 49);
    return rows;
  };
  const auto render = [&](GrayscaleMode mode, uint8_t mask, bool factory) {
    std::fill(msb.begin(), msb.end(), mask);
    driver.beginGrayscale(bus, bw.data(), mode, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
    driver.displayGray(bus, bw.data(), false, nullptr, factory);
    return bank();
  };
  const auto quality = render(GrayscaleMode::Direct, 0, false);
  assert(quality[2] != quality[3]);
  assert(render(GrayscaleMode::Absolute, 0, false) == quality);
  for (uint8_t variant : {0x02, 0x68, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    for (uint8_t mask : {0x00, 0x01, 0x03}) {
      const auto text = render(GrayscaleMode::Overlay, mask, false);
      assert(text != quality);
      assert(text[2] == text[3]);
      const uint8_t timing = variant == 0x02 ? 2 : 3;
      for (unsigned t = 0; t < 5; ++t) {
        Bytes expected = {1, 2, timing, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1};
        expected.resize(49, 0);
        if (t == 1) expected[3] |= 0x40;
        if (t == 2 || t == 3) expected[2] |= 0x80;
        if (t == 4) expected[3] |= 0x80;
        assert(text[t] == expected);
      }
    }
    assert(render(GrayscaleMode::Overlay, 0x07, false) == quality);
    assert(render(GrayscaleMode::Overlay, 0, true) == quality);
    // An image pass must not leave the next sparse page on the quality bank.
    assert(render(GrayscaleMode::Overlay, 0x01, false) != quality);
  }
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  free(driver._grayBase);
}

static void testStickyAbsolute() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::Sticky;
  auto& driver = static_cast<Ssd1677Driver&>(ssd1677Driver());
  assert(driver._cfg.grayPowerUpFirst);
  assert(driver._cfg.fullSeqOverride == 0xF7);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  display._bus.clear();
  assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
  assert(display._bus.writes.empty());
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer(false);
  assert(lastRegister(display._bus, 0x22) == 0xCC);
  assert(driver._isScreenOn);
  display.cleanupGrayscaleBuffers(bw.data());
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  assert(lastRegister(display._bus, 0x22) == 0xF7);
  display.releaseBuffers();
}

int main(int argc, char**) {
  if (argc > 1) {
    testStickyAbsolute();
    std::puts("Sticky absolute capability, activation, power-down and B/W recovery passed");
    return 0;
  }
  testUc8179GrayShadeSplit();
  testUc8279X4WaveformSelection();
  testDirectSleep<Uc8179Driver>();
  testDirectSleep<Uc8279X4Driver>();
  testUltraChipAbsolute<Uc8179Driver>(true, 0, false);
  for (uint8_t variant : {0x02, 0x68, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    testUltraChipAbsolute<Uc8279X4Driver>(false, 120, true);
  }
  testAbsolutePipeline();
  testCapabilities();
  testStream<Uc8179Driver>(true,0);
  testStream<Uc8279X4Driver>(false,120);
  testSsd();
  testAsyncFrame<Uc8179Driver>();
  testAsyncFrame<Uc8279X4Driver>();
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  testGrayCleanupBaseline();
#endif
  std::puts("Pro plane bytes, transaction counts, clean refreshes, power state and async frame ownership passed");
}
