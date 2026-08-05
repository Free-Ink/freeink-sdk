#pragma once

// USB Mass Storage ("USB Transfer" mode): exposes a block device (the SD card)
// to a USB host as a removable disk, so the user can copy files over USB without
// removing the card. Mirrors the Xteink stock firmware, which drives esp-idf's
// TinyUSB MSC over the same sdmmc_card_t the app FAT-mounts. Here it's built on
// arduino-esp32's USBMSC class, whose read/write callbacks route straight to the
// block device's sector I/O.
//
// OPT-IN per board via FREEINK_CAP_USB_MSC (see BoardConfig.h) — requires the
// build's USB stack in OTG mode (ARDUINO_USB_MODE=0 + CONFIG_TINYUSB_MSC_ENABLED).
// When the capability is off this compiles to a trivial stub and links no USB code.
//
// Lifecycle (caller — e.g. a "USB Transfer" UI mode):
//   1. Ensure the SD card is mounted, then SUSPEND all app filesystem use (the
//      host owns the card while active — concurrent app writes corrupt the FS).
//   2. begin(blockDevice). USB enumerates as a disk.
//   3. Poll hostConnected(): once it drops (host ejected / cable pulled), end()
//      and re-init the app's filesystem (the stock firmware flushes deferred
//      writes and remounts here).

#include <BoardConfig.h>

#if FREEINK_CAP_USB_MSC
#include <SdFat.h>  // FsBlockDeviceInterface (requires USE_BLOCK_DEVICE_INTERFACE=1)

namespace freeink {

class UsbMassStorage {
 public:
  // Expose `dev` (512-byte sectors, count from dev->sectorCount()) over USB-MSC
  // and start the USB device. Returns false if already active or the card is
  // absent. The caller must have suspended its own FS use first.
  bool begin(FsBlockDeviceInterface* dev);

  // Stop MSC and tear the USB device down. The caller re-inits its FS after.
  void end();

  bool active() const { return _active; }

  // True while a USB host currently has the disk mounted. Transitions to false
  // on unplug / host eject — the caller's cue to end() and remount the FS.
  bool hostConnected() const;

 private:
  bool _active = false;
};

}  // namespace freeink

#else  // !FREEINK_CAP_USB_MSC — stub, no USB/TinyUSB code linked

namespace freeink {
class UsbMassStorage {
 public:
  bool active() const { return false; }
  bool hostConnected() const { return false; }
};
}  // namespace freeink

#endif  // FREEINK_CAP_USB_MSC
