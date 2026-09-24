#include <cassert>
#include <iostream>
#include <vector>
#include "driver/Uc8279Driver.h"
#include "lut/Uc8279X3Luts.h"

int main() {
  using namespace freeink;
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
  std::cout << "PASS: UC8279 overlay/absolute LUT mapping, unchanged planes, cleanup and wake\n";
}
