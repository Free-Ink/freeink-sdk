#include "UsbMassStorage.h"

#if FREEINK_CAP_USB_MSC

#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

extern "C" bool tud_mounted(void);
extern "C" bool tud_suspended(void);
extern "C" bool tud_disconnect(void);

namespace freeink {
namespace {

USBMSC gMsc;
// The mutex covers admission checks, shared scratch space and every block call.
// Teardown waits here until the last accepted operation has actually finished.
std::mutex gIoMutex;
std::atomic<bool> gAccepting{false};
UsbMassStorage* gOwner = nullptr;
FsBlockDeviceInterface* gDev = nullptr;
uint32_t gConfiguredSectors = 0;
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
  std::lock_guard<std::mutex> lock(gIoMutex);
  if (!gAccepting.load()) return -1;
  auto* const dev = gDev;
  auto* const owner = gOwner;
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
  std::lock_guard<std::mutex> lock(gIoMutex);
  if (!gAccepting.load()) return -1;
  auto* const dev = gDev;
  auto* const owner = gOwner;
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

  // One checked sync per USB buffer, not per sector. SDCardManager uses shared
  // SPI (writeSectors already stops the transfer) or synchronous SDMMC, so
  // their sync is a no-op here. Keep it for other block devices: Arduino USBMSC
  // has no SYNCHRONIZE CACHE hook. Dedicated SPI must stop its stream before we
  // can acknowledge a checked write; deferring that until eject would weaken
  // it.
  if (!dev->syncDevice()) {
    if (owner) owner->markIoError();
    return -1;
  }
  if (owner) owner->markAccessed();
  return static_cast<int32_t>(bufsize);
}

bool mscStartStop(uint8_t /*power_condition*/, bool start, bool load_eject) {
  std::lock_guard<std::mutex> lock(gIoMutex);
  if (!gAccepting.load() || !gDev || !gOwner) return false;
  if (load_eject && !start) {
    if (!gDev->syncDevice()) {
      gOwner->markIoError();
      return false;
    }
    gAccepting.store(false);
    gOwner->markEjected();
  }
  return true;
}

}  // namespace

bool UsbMassStorage::begin(FsBlockDeviceInterface* dev) {
  std::lock_guard<std::mutex> lock(gIoMutex);
  if (_active || gOwner || !dev || dev->sectorCount() == 0) return false;
  const uint32_t sectors = dev->sectorCount();
  // Arduino reads its callback table outside our mutex. Configure it once and
  // keep it stable for the process lifetime, including failed startup/retries.
  if (gConfiguredSectors && gConfiguredSectors != sectors) {
    if (Serial) {
      Serial.printf(
          "USB MSC capacity changed (%lu -> %lu sectors); restart "
          "before using a different-sized card\n",
          static_cast<unsigned long>(gConfiguredSectors), static_cast<unsigned long>(sectors));
    }
    return false;
  }
  if (!gConfiguredSectors) {
    gMsc.vendorID("FreeInk");
    gMsc.productID("SD Card");
    gMsc.productRevision("1.0");
    gMsc.mediaPresent(true);
    gMsc.isWritable(true);
    gMsc.onStartStop(mscStartStop);
    gMsc.onRead(mscRead);
    gMsc.onWrite(mscWrite);
    if (!gMsc.begin(sectors, kBlockSize)) return false;
    gConfiguredSectors = sectors;
  }
  if (!USB.begin()) return false;
  gDev = dev;
  gOwner = this;
  _state.store(UsbMassStorageState::WaitingForHost);
  _hostSeen.store(false);
  _active = true;
  gAccepting.store(true);
  return true;
}

bool UsbMassStorage::end() {
  if (!_active) return true;
  gAccepting.store(false);
  tud_disconnect();
  std::lock_guard<std::mutex> lock(gIoMutex);
  // An operation that passed admission has returned before we can take this
  // lock. Queued callbacks see admission closed and never touch the card.
  if (!gDev || !gDev->syncDevice()) {
    markIoError();
    return false;
  }
  _active = false;
  gOwner = nullptr;
  gDev = nullptr;
  // Leave Arduino's callback pointers intact. A callback already dispatched
  // inside its USBMSC wrapper must remain callable after disconnect.
  _state.store(UsbMassStorageState::Idle);
  _hostSeen.store(false);
  return true;
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

bool UsbMassStorage::disconnectHost() const {
  if (!_active) return false;
  gAccepting.store(false);
  return tud_disconnect();
}

bool UsbMassStorage::hostSuspended() const { return _active && tud_mounted() && tud_suspended(); }

void UsbMassStorage::markAccessed() const {
  auto current = _state.load();
  while (current != UsbMassStorageState::Ejected && current != UsbMassStorageState::IoError) {
    if (_state.compare_exchange_weak(current, UsbMassStorageState::Accessed)) return;
  }
}

void UsbMassStorage::markEjected() const { _state.store(UsbMassStorageState::Ejected); }

void UsbMassStorage::markIoError() const {
  gAccepting.store(false);
  _hostSeen.store(true);
  _state.store(UsbMassStorageState::IoError);
}

}  // namespace freeink

#endif  // FREEINK_CAP_USB_MSC
