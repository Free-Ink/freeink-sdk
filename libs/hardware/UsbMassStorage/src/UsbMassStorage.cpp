#include "UsbMassStorage.h"

#if FREEINK_CAP_USB_MSC

#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>

#include <algorithm>
#include <atomic>
#include <cstring>

extern "C" bool tud_mounted(void);
extern "C" bool tud_disconnect(void);

namespace freeink {
namespace {

USBMSC gMsc;
std::atomic<UsbMassStorage*> gOwner{nullptr};
std::atomic<FsBlockDeviceInterface*> gDev{nullptr};
constexpr uint16_t kBlockSize = 512;
uint8_t gSectorScratch[kBlockSize];

bool isRangeValid(FsBlockDeviceInterface* dev, const uint32_t lba, const uint32_t offset, const uint32_t bufsize) {
  if (!dev || bufsize == 0) return false;
  const uint64_t totalBytes = static_cast<uint64_t>(dev->sectorCount()) * kBlockSize;
  const uint64_t start = static_cast<uint64_t>(lba) * kBlockSize + offset;
  const uint64_t end = start + bufsize;
  return end >= start && end <= totalBytes;
}

int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  auto* const dev = gDev.load();
  auto* const owner = gOwner.load();
  if (!buffer || !isRangeValid(dev, lba, offset, bufsize)) {
    if (owner) owner->markIoError();
    return -1;
  }

  const uint64_t start = static_cast<uint64_t>(lba) * kBlockSize + offset;
  auto* output = static_cast<uint8_t*>(buffer);
  uint32_t remaining = bufsize;
  uint64_t cursor = start;

  if ((offset % kBlockSize) == 0 && (bufsize % kBlockSize) == 0) {
    const size_t sectors = bufsize / kBlockSize;
    if (!dev->readSectors(static_cast<Sector_t>(start / kBlockSize), output, sectors)) {
      if (owner) owner->markIoError();
      return -1;
    }
  } else {
    while (remaining > 0) {
      const auto sector = static_cast<Sector_t>(cursor / kBlockSize);
      const size_t within = static_cast<size_t>(cursor % kBlockSize);
      const size_t count = std::min<size_t>(kBlockSize - within, remaining);
      if (!dev->readSector(sector, gSectorScratch)) {
        if (owner) owner->markIoError();
        return -1;
      }
      memcpy(output, gSectorScratch + within, count);
      output += count;
      cursor += count;
      remaining -= count;
    }
  }

  if (owner) owner->markAccessed();
  return static_cast<int32_t>(bufsize);
}

int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  auto* const dev = gDev.load();
  auto* const owner = gOwner.load();
  if (!buffer || !isRangeValid(dev, lba, offset, bufsize)) {
    if (owner) owner->markIoError();
    return -1;
  }

  const uint64_t start = static_cast<uint64_t>(lba) * kBlockSize + offset;
  auto* input = buffer;
  uint32_t remaining = bufsize;
  uint64_t cursor = start;

  if ((offset % kBlockSize) == 0 && (bufsize % kBlockSize) == 0) {
    const size_t sectors = bufsize / kBlockSize;
    if (!dev->writeSectors(static_cast<Sector_t>(start / kBlockSize), input, sectors)) {
      if (owner) owner->markIoError();
      return -1;
    }
  } else {
    while (remaining > 0) {
      const auto sector = static_cast<Sector_t>(cursor / kBlockSize);
      const size_t within = static_cast<size_t>(cursor % kBlockSize);
      const size_t count = std::min<size_t>(kBlockSize - within, remaining);
      if (!dev->readSector(sector, gSectorScratch)) {
        if (owner) owner->markIoError();
        return -1;
      }
      memcpy(gSectorScratch + within, input, count);
      if (!dev->writeSector(sector, gSectorScratch)) {
        if (owner) owner->markIoError();
        return -1;
      }
      input += count;
      cursor += count;
      remaining -= count;
    }
  }

  if (owner) owner->markAccessed();
  return static_cast<int32_t>(bufsize);
}

bool mscStartStop(uint8_t /*power_condition*/, bool start, bool load_eject) {
  if (load_eject && !start) {
    if (auto* const owner = gOwner.load()) owner->markEjected();
  }
  return true;
}

}  // namespace

bool UsbMassStorage::begin(FsBlockDeviceInterface* dev) {
  if (_active || !dev || dev->sectorCount() == 0) return false;

  gDev.store(dev);
  gOwner.store(this);
  _state.store(UsbMassStorageState::WaitingForHost);
  _hostSeen.store(false);
  gMsc.vendorID("FreeInk");
  gMsc.productID("SD Card");
  gMsc.productRevision("1.0");
  gMsc.mediaPresent(true);
  gMsc.isWritable(true);
  gMsc.onStartStop(mscStartStop);
  gMsc.onRead(mscRead);
  gMsc.onWrite(mscWrite);
  if (!gMsc.begin(dev->sectorCount(), kBlockSize)) {
    gMsc.end();
    gOwner.store(nullptr);
    gDev.store(nullptr);
    _state.store(UsbMassStorageState::Idle);
    return false;
  }
  if (!USB.begin()) {
    gMsc.end();
    gOwner.store(nullptr);
    gDev.store(nullptr);
    _state.store(UsbMassStorageState::Idle);
    return false;
  }
  _active = true;
  return true;
}

void UsbMassStorage::end() {
  if (!_active) return;
  gMsc.end();
  gOwner.store(nullptr);
  gDev.store(nullptr);
  _active = false;
  _state.store(UsbMassStorageState::Idle);
  _hostSeen.store(false);
}

UsbMassStorageState UsbMassStorage::state() const {
  if (!_active) return UsbMassStorageState::Idle;
  const auto current = _state.load();
  if (current == UsbMassStorageState::Ejected) return current;

  if (!tud_mounted()) {
    return _hostSeen.load() ? UsbMassStorageState::Disconnected : UsbMassStorageState::WaitingForHost;
  }

  // Keep the error visible while the host is mounted, but report a later cable
  // removal so the application can safely reclaim its raw storage session.
  if (current == UsbMassStorageState::IoError) return current;

  _hostSeen.store(true);
  auto expected = UsbMassStorageState::WaitingForHost;
  _state.compare_exchange_strong(expected, UsbMassStorageState::Connected);
  return _state.load();
}

bool UsbMassStorage::hostConnected() const {
  const auto current = state();
  return current == UsbMassStorageState::Connected || current == UsbMassStorageState::Accessed;
}

bool UsbMassStorage::disconnectHost() const { return _active && tud_disconnect(); }

void UsbMassStorage::markAccessed() const {
  auto current = _state.load();
  while (current != UsbMassStorageState::Ejected && current != UsbMassStorageState::IoError) {
    if (_state.compare_exchange_weak(current, UsbMassStorageState::Accessed)) return;
  }
}

void UsbMassStorage::markEjected() const { _state.store(UsbMassStorageState::Ejected); }

void UsbMassStorage::markIoError() const {
  _hostSeen.store(true);
  _state.store(UsbMassStorageState::IoError);
}

}  // namespace freeink

#endif  // FREEINK_CAP_USB_MSC
