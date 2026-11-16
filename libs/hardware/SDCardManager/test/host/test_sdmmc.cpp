#include <SDCardManager.h>
#include <driver/gpio.h>

#include <cassert>
#include <cstdio>

#include "../../src/SdmmcBlockDevice.h"

namespace {
unsigned deviceEnds = 0;
}

// Only the physical ESP-IDF transport is faked. Compile the production SDMMC
// manager's mount, sync, shutdown and detach paths against the actual API.
namespace freeink {
bool SdmmcBlockDevice::begin(const BoardConfig::SdmmcPins&) {
  _card = this;
  _dmaBuffer = nullptr;
  return true;
}
void SdmmcBlockDevice::end() {
  _card = nullptr;
  _dmaBuffer = nullptr;
  ++deviceEnds;
}
bool SdmmcBlockDevice::readSectors(Sector_t, uint8_t*, size_t) { return false; }
bool SdmmcBlockDevice::writeSectors(Sector_t, const uint8_t*, size_t) { return false; }
Sector_t SdmmcBlockDevice::sectorCount() { return _card && !_dmaBuffer ? 1024 : 0; }
}  // namespace freeink

int main() {
  auto& manager = SDCardManager::getInstance();
  assert(manager.shutdown());  // never-mounted shutdown is harmless
  assert(manager.begin());
  for (int fault = 0; fault < 3; ++fault) {
    fakeSd::rootOpenFails = fault == 0;
    fakeSd::rootSyncFails = fault == 1;
    fakeSd::rootCloseFails = fault == 2;
    const auto closes = fakeSd::closes;
    assert(!manager.syncFilesystem());
    assert(fakeSd::closes == closes + (fault == 0 ? 0 : 1));
    assert(!manager.shutdown());
    assert(manager.detachFilesystemForRawAccess() == nullptr);
    assert(manager.ready());
    assert(fakeSd::volumeEnds == 0 && deviceEnds == 0 && floatedPins == 0);
  }
  fakeSd::rootOpenFails = fakeSd::rootSyncFails = fakeSd::rootCloseFails = false;
  assert(manager.shutdown());
  assert(!manager.ready());
  assert(fakeSd::volumeEnds == 1 && deviceEnds == 1 && floatedPins == 6);
  assert(manager.shutdown());
  assert(deviceEnds == 1);
  assert(manager.begin());
  const auto stopped = deviceEnds;
  assert(manager.detachFilesystemForRawAccess() != nullptr);
  assert(!manager.ready() && fakeSd::volumeEnds == 2);
  assert(deviceEnds == stopped);  // raw owner keeps its transport alive
  std::puts("SDMMC checked shutdown and raw-handoff tests passed");
}
