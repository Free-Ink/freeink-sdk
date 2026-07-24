#include "UsbMassStorage.h"

#if FREEINK_CAP_USB_MSC

#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>

// TinyUSB device-mounted state. Declared here to avoid pulling the full tusb
// header set into this TU; it's a plain C symbol from the arduino-esp32 USB stack.
extern "C" bool tud_mounted(void);

namespace freeink {
namespace {

USBMSC gMsc;
FsBlockDeviceInterface* gDev = nullptr;
constexpr uint16_t kBlockSize = 512;

// USBMSC read/write callbacks. TinyUSB passes a byte `offset` within the LBA for
// scatter transfers; in practice it is 0 and `bufsize` is a whole number of
// sectors. Route to the block device's sector I/O (SdmmcBlockDevice already
// bounces through DMA-capable memory).
int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (gDev == nullptr || (bufsize % kBlockSize) != 0) return -1;
  const uint32_t ns = bufsize / kBlockSize;
  if (!gDev->readSectors(lba + offset / kBlockSize, static_cast<uint8_t*>(buffer), ns)) return -1;
  return static_cast<int32_t>(bufsize);
}

int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (gDev == nullptr || (bufsize % kBlockSize) != 0) return -1;
  const uint32_t ns = bufsize / kBlockSize;
  if (!gDev->writeSectors(lba + offset / kBlockSize, buffer, ns)) return -1;
  return static_cast<int32_t>(bufsize);
}

// Accept host START/STOP UNIT (spin-up/eject) — nothing to gate; the medium is
// always present while active.
bool mscStartStop(uint8_t /*power_condition*/, bool /*start*/, bool /*load_eject*/) { return true; }

}  // namespace

bool UsbMassStorage::begin(FsBlockDeviceInterface* dev) {
  if (_active || dev == nullptr) return false;
  const uint32_t sectors = dev->sectorCount();
  if (sectors == 0) return false;  // no card / not mounted

  gDev = dev;
  gMsc.vendorID("FreeInk");
  gMsc.productID("SD Card");
  gMsc.productRevision("1.0");
  gMsc.mediaPresent(true);
  gMsc.isWritable(true);
  gMsc.onStartStop(mscStartStop);
  gMsc.onRead(mscRead);
  gMsc.onWrite(mscWrite);
  if (!gMsc.begin(sectors, kBlockSize)) {
    gDev = nullptr;
    return false;
  }
  USB.begin();
  _active = true;
  return true;
}

void UsbMassStorage::end() {
  if (!_active) return;
  gMsc.end();
  gDev = nullptr;
  _active = false;
}

bool UsbMassStorage::hostConnected() const { return _active && tud_mounted(); }

}  // namespace freeink

#endif  // FREEINK_CAP_USB_MSC
