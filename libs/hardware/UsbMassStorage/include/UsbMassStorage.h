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
  // True while the USB bus is idle (no SOF for >3 ms), which is what the device
  // sees when the cable is pulled.
  //
  // Needed because tud_mounted() CANNOT report an unplug on the ESP32-S3:
  // Arduino's tinyusb init passes otg_io_conf = NULL (esp32-hal-tinyusb.c), so
  // no VBUS line is routed to the OTG core and IDF forces B-session-valid on.
  // The core therefore never detects session end, and state() stays Connected
  // forever after the cable is gone. Bus suspend is detected by the core itself
  // and is unaffected.
  //
  // A host suspending an idle bus looks identical, so this is a HINT, not a
  // verdict: callers must require it to persist before acting on it, and should
  // prefer a physical VBUS reading where the board has one.
  bool hostSuspended() const;
  // Soft-disconnect the USB device from the host. Call from application/task
  // context, never from an MSC callback; end() still owns final teardown.
  bool disconnectHost() const;

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
  bool disconnectHost() const { return false; }
  bool hostSuspended() const { return false; }
};
}  // namespace freeink

#endif  // FREEINK_CAP_USB_MSC
