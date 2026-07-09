#pragma once

// FreeInk SDK — USB mass-storage gadget (singleton).
//
// Device-mode MSC on the ESP32-S3/S2 USB-OTG port: exposes a wear-levelled
// internal-flash FAT partition to a host PC as a removable USB disk, alongside
// the TinyUSB CDC console (composite device — serial logs and esptool flashing
// keep working on the same cable).
//
// Ownership model — MSC hands the host a raw BLOCK device, so the host's FAT
// driver and the firmware's FATFS must never both own the volume. The LUN is
// always enumerated but reports "no media" until the consumer hands the disk
// over. The consumer flow:
//
//   UsbMsc.begin("MyDevice");                  // once, in setup()
//   ...
//   // main loop:
//   UsbMscGadget::Event ev;
//   while (UsbMsc.popEvent(ev)) {
//     if (ev == Event::HostConnected) {        // a real host configured us —
//       myFs.end();                            //   charging bricks never do
//       UsbMsc.startSession("ffat");           // wl_mount + media present
//       showUsbScreen();
//     } else {                                 // MediaEjected / HostDisconnected
//       UsbMsc.endSession();                   // media absent + wl_unmount
//       myFs.begin();                          // take the volume back
//       showBrowser();
//     }
//   }
//
// Block layer: reads/writes are served through the wear-levelling layer
// (wl_read / wl_erase_range + wl_write) — never raw partition bytes, which
// would bypass the WL sector remap and corrupt the volume. The MSC block size
// is the WL sector size (4096 with the prebuilt Arduino libs; equal to the
// prebuilt CFG_TUD_MSC_BUFSIZE, so every transfer is one whole sector).
//
// Capability-gated: the real TinyUSB implementation compiles only when
// FREEINK_CAP_USB_MSC is set AND the build runs the TinyUSB/OTG stack
// (-DARDUINO_USB_MODE=0); otherwise every method links a stub so callers need
// no #ifdefs and no USB code is pulled in. This header is deliberately
// USB-header-free so non-OTG builds (ESP32-C3, USB_MODE=1 debug envs) never
// see the TinyUSB stack.
//
// NOTE: the OTG stack displaces the S3's USB-Serial-JTAG console/debugger on
// the shared port. Keep a -DARDUINO_USB_MODE=1 build env for OpenOCD/JTAG and
// crash-proof console work — see docs/usb-msc-gadget.md.

#include <stdint.h>

// Canonical default lives in BoardConfig.h's capability section; this fallback
// keeps the header standalone.
#ifndef FREEINK_CAP_USB_MSC
#define FREEINK_CAP_USB_MSC 0
#endif

namespace freeink {

class UsbMscGadget {
 public:
  // Events are queued from USB-stack task context; drain with popEvent() from
  // the main loop.
  enum class Event : uint8_t {
    HostConnected = 1,  // host configured the device (fires on plug-in to a PC,
                        // on boot while already plugged in, and on host resume)
    HostDisconnected,   // bus stopped or suspended (unplug, host sleep)
    MediaEjected,       // host ejected the disk (SCSI Start/Stop Unit)
  };

  // Set up the MSC LUN (no media) + event plumbing and make sure the USB
  // stack is running. `product`/`vendor` are the SCSI INQUIRY strings shown
  // by host OSes (max 16 / 8 chars). If the device is already enumerated by a
  // host (plugged in during boot), a HostConnected event is queued
  // immediately. Call once in setup(). Returns false on stub builds.
  bool begin(const char* product = "FreeInk", const char* vendor = "FreeInk");

  // Hand the wear-levelled FAT partition `partitionLabel` to the host:
  // wl_mount()s it, publishes its geometry, and flips MSC media-present. The
  // consumer MUST have unmounted its own filesystem first (e.g. FFat.end()).
  // No-op true if a session is already active.
  bool startSession(const char* partitionLabel = "ffat");

  // Take the disk back: flips media-absent and wl_unmount()s. The consumer
  // remounts its filesystem afterwards. Safe to call when no session is open.
  void endSession();

  bool sessionActive() const;

  // True while a host has the device configured (independent of any session).
  bool hostConnected() const;

  // Pop the next queued event. Returns false when none is pending.
  bool popEvent(Event& out);

  static UsbMscGadget& getInstance() { return instance; }

 private:
  static UsbMscGadget instance;
};

}  // namespace freeink

#define UsbMsc ::freeink::UsbMscGadget::getInstance()
