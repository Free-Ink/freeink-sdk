#pragma once

// USB Mass Storage ("USB Transfer" mode): exposes a block device (the SD card)
// to a USB host as a removable disk. The owner must suspend all filesystem use
// before begin() and must reboot/remount after the host ejects or disconnects.

#include <BoardConfig.h>

#include <atomic>

namespace freeink {

enum class UsbMassStorageState : uint8_t {
  Idle,
  WaitingForHost,
  Connected,
  Accessed,
  Ejected,
  Disconnected,
  IoError,
};

}  // namespace freeink

#if FREEINK_CAP_USB_MSC
#include <SdFat.h>

namespace freeink {

class UsbMassStorage {
 public:
  bool begin(FsBlockDeviceInterface* dev);
  void end();

  bool active() const { return _active; }
  UsbMassStorageState state() const;
  bool hostConnected() const;

  // Called by the TinyUSB callbacks to publish the most recent host event.
  void markAccessed() const;
  void markEjected() const;
  void markIoError() const;

 private:
  // TinyUSB callbacks and the app loop run in separate FreeRTOS tasks on the
  // S3, so publish lifecycle updates atomically rather than relying on a
  // best-effort byte-sized write.
  mutable std::atomic<UsbMassStorageState> _state{UsbMassStorageState::Idle};
  mutable std::atomic<bool> _hostSeen{false};
  bool _active = false;
};

}  // namespace freeink

#else

namespace freeink {
class UsbMassStorage {
 public:
  bool active() const { return false; }
  UsbMassStorageState state() const { return UsbMassStorageState::Idle; }
  bool hostConnected() const { return false; }
};
}  // namespace freeink

#endif  // FREEINK_CAP_USB_MSC
