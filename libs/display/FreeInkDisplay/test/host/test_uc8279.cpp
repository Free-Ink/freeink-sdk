#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Uc8279Driver.h"
#undef private
#include "src/lut/Uc8279X3Luts.h"

int main() {
  using namespace freeink;
  // V6.5.5 includes the control tail of the fifth group even with zero drive durations.
  for (const auto& row : kUc8279X3_Xth4) {
    assert(row[28] == 1 && row[33] == 1 && row[34] == 1);
    for (unsigned i = 29; i < 33; ++i) assert(row[i] == 0);
  }
  constexpr size_t n = 792 / 8 * 528;
  EpdBus bus;
  Uc8279Driver driver;
  std::vector<uint8_t> bw(n, 0x88), lsb(n, 0xAA), msb(n, 0xCC);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(driver.grayscaleCapabilities().stripUploads && !driver.grayscaleCapabilities().asyncBase);
  driver.begin(bus);
  driver.displayGrayscaleBase(bus, bw.data(), RefreshMode::Half, false);
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  // Inputs are passed unchanged regardless of gray-pixel coverage.
  assert(bus.oldPlane == lsb && bus.newPlane == msb);
  driver.displayGray(bus, bw.data(), false, nullptr, false);
  assert(bus.rawRegisters == std::vector<uint8_t>({0x20, 0x23, 0x22, 0x21, 0x24}));
  assert(bus.lastBank == std::vector<uint8_t>(kUc8279X3_XtfAa[0], kUc8279X3_XtfAa[0] + 49));

  for (bool turnOff : {false, true}) {
    driver.displayGrayscaleBase(bus, bw.data(), RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    driver.displayGray(bus, bw.data(), turnOff, nullptr, true);
    assert(bus.rawRegisters == std::vector<uint8_t>({0x20, 0x24, 0x22, 0x23, 0x21}));
    assert(bus.lastBank == std::vector<uint8_t>(kUc8279X3_Xth4[0], kUc8279X3_Xth4[0] + 49));
    // Cleanup must not clear the required physical rebase after an absolute pass.
    driver.cleanupGrayscaleBuffers(bus, bw.data());
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, true);
    assert(bus.oldPlane == bw && bus.newPlane == bw);
    assert(bus.lastBwBank == std::vector<uint8_t>(kUc8279X3_BwGc[0]+1, kUc8279X3_BwGc[0]+43));
  }
  for (auto fallback : {RefreshMode::Half, RefreshMode::Fast}) {
    const auto before = bus.refreshes;
    driver.beginGrayscale(bus, bw.data(), GrayscaleMode::Direct, fallback, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    assert(bus.refreshes == before);
    assert(bus.oldPlane == lsb && bus.newPlane == msb);
    driver.displayGray(bus, bw.data(), false, nullptr, true);
    assert(bus.refreshes == before + 1);
    assert(bus.lastBank == std::vector<uint8_t>(kUc8279X3_Xth4[0], kUc8279X3_Xth4[0] + 49));
    driver.cleanupGrayscaleBuffers(bus, bw.data());
    driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, true);
    assert(bus.oldPlane == bw && bus.newPlane == bw);
  }
  driver.deepSleep(bus);
  driver.begin(bus);
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(bus.oldPlane == bw && bus.newPlane == bw);

  // Facade parity on UC8279: powerOffBetweenFrames() is a no-op, Dark->Light
  // displayGrayscaleBase(FAST_REFRESH, true) honors turnOffScreen, and Absolute
  // writeGrayscalePlaneGateStream() returns false cleanly so strip fallback works.
  FreeInkDisplay facade(0, 0, 0, 0, 0, 0);
  Uc8279Driver facDriver;
  facade.setDisplayX3();
  facade.begin();
  facade._driver = &facDriver;
  facDriver.begin(facade._bus);
  std::memcpy(facade.getFrameBuffer(), bw.data(), bw.size());
  facade.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
  const unsigned pofBeforeNoop = facade._bus.powerOffs;
  facade.powerOffBetweenFrames();
  assert(facade._bus.powerOffs == pofBeforeNoop);

  facade.setInverted(true);
  facade.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
  facade.setInverted(false);
  const unsigned pofBeforeDarkExit = facade._bus.powerOffs;
  facade.displayGrayscaleBase(FreeInkDisplay::FAST_REFRESH, /*turnOffScreen=*/true);
  assert(facade._bus.powerOffs == pofBeforeDarkExit + 1);
  assert(!facade._inversionDirty);

  assert(facade.displayGrayscaleBase(GrayscaleMode::Absolute));
  auto dummyCb = [](void*, const uint8_t** out) -> size_t {
    static const uint8_t kByte = 0xAA;
    *out = &kByte;
    return 1;
  };
  assert(!facade.writeGrayscalePlaneGateStream(FreeInkDisplay::GRAY_PLANE_LSB, nullptr, +dummyCb));
  assert(facade._grayRows[0] == 0 && facade._grayRows[1] == 0 && !facade._grayPassFailed);
  for (uint16_t y = 0; y < 528; y += 88) {
    facade.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data() + size_t(y) * 99, y, 88);
    facade.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_MSB, msb.data() + size_t(y) * 99, y, 88);
  }
  const unsigned refBeforeAbs = facade._bus.refreshes;
  facade.displayGrayBuffer(true);
  assert(facade._bus.refreshes == refBeforeAbs + 1);
  facade.releaseBuffers();
  std::cout << "PASS: UC8279 overlay/absolute LUT mapping, unchanged planes, cleanup and wake\n";
}
