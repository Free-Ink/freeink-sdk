# UsbMscGadget — device-mode USB mass storage

`libs/hardware/UsbMscGadget` turns an ESP32-S3/S2 board into a removable USB
disk: plug it into a PC and the device's storage appears as a drive — either
a wear-levelled internal-flash FAT partition (e.g. an `ffat` documents
partition) or the board's **microSD card** (Sticky, M5 PaperColor, Murphy,
de-link). The gadget is a composite device — the TinyUSB CDC console stays
alive next to the MSC interface, so serial logs and esptool flashing keep
working on the same cable.

## Requirements

- **ESP32-S3 or S2** (USB-OTG peripheral; the C3 has none).
- **TinyUSB stack**: `-DARDUINO_USB_MODE=0`. This displaces the S3's
  USB-Serial-JTAG peripheral on the shared USB pins: OpenOCD/JTAG over USB and
  the crash-proof ROM console are unavailable in such a build. Keep a sibling
  env with `-DARDUINO_USB_MODE=1` (and `-DFREEINK_CAP_USB_MSC=0`, which turns
  this lib into stubs) for debugging sessions. ROM download mode (hold BOOT
  while plugging in) always works regardless.
- **`-DFREEINK_CAP_USB_MSC=1`** plus the lib in `lib_deps` (see
  `platformio.sample.ini`). Builds without the cap (or on USB_MODE=1) link
  stub bodies — callers need no `#ifdef`s.
- A `data/fat` partition in the partition table (the same one FFat mounts).

## Ownership model

MSC hands the host a raw **block device**. The host's FAT driver and the
firmware's FATFS must never both own the volume — two writers with independent
sector caches corrupt it. The gadget therefore keeps the LUN enumerated but
media-absent until the consumer explicitly hands the disk over, and the
consumer must unmount its filesystem first:

```cpp
UsbMsc.begin("MyDevice");                    // setup(): LUN up, no media

// main loop:
freeink::UsbMscGadget::Event ev;
while (UsbMsc.popEvent(ev)) {
  using Event = freeink::UsbMscGadget::Event;
  if (ev == Event::HostConnected && !UsbMsc.sessionActive()) {
    saveOpenDocument();                      // consumer policy
    FFat.end();                              // release the volume
    if (UsbMsc.startSession("ffat")) showUsbScreen();
    else FFat.begin();                       // handover failed — take it back
  } else if (ev == Event::MediaEjected || ev == Event::HostDisconnected) {
    if (UsbMsc.sessionActive()) {
      UsbMsc.endSession();
      if (FFat.begin()) showBrowser();
      else showStorageErrorScreen();         // host left the FAT unreadable
    }
  }
}
```

`HostConnected` fires when a host **configures** the device — on plug-in to a
PC, at boot while already plugged in, and on host resume. Charging bricks
never configure, so charging does not trigger transfer mode.

Consumers should also refuse to deep-sleep while `sessionActive()`.

## SD-card sessions

Boards with a microSD slot serve the card instead of (or as well as) internal
flash. SDCardManager provides the raw side: `beginRaw()` releases the
firmware's FAT view and re-initializes the card for sector access;
`sectorCount()` / `readRawSectors()` / `writeRawSectors()` serve 512-byte
sectors. Wire them into a `BlockSource`:

```cpp
static bool sdRead(void*, uint32_t s, uint8_t* d, size_t n) { return SdMan.readRawSectors(s, d, n); }
static bool sdWrite(void*, uint32_t s, const uint8_t* d, size_t n) { return SdMan.writeRawSectors(s, d, n); }

// HostConnected:
showUsbScreen();                 // paint FIRST on shared-SPI boards (below)
if (SdMan.beginRaw()) {
  freeink::UsbMscGadget::BlockSource src{SdMan.sectorCount(), 512, sdRead, sdWrite, nullptr};
  if (!UsbMsc.startSession(src)) SdMan.begin();   // handover failed — remount
}

// MediaEjected / HostDisconnected:
UsbMsc.endSession();
SdMan.begin();                   // remount the FAT view for the app
```

Rules specific to SD sessions:

- **Shared SPI bus (Sticky, M5 PaperColor, M5Paper):** MSC callbacks run on
  the TinyUSB task while the app task owns the e-paper on the same bus. Paint
  the "connected to computer" screen **before** `startSession()` and leave
  the display untouched until the session ends — the e-ink image persists on
  its own. Boards with the SD on its own bus have no such constraint.
- **Card power** stays up for the whole session; `beginRaw()` runs the same
  power-rail bring-up as `begin()`.
- **Physical card removal** mid-session surfaces as IO errors to the host
  (same as yanking a card out of any reader). The firmware recovers on the
  next `SdMan.begin()`.
- The host sees the whole card, whatever filesystem is on it — exFAT cards
  work over USB even though the firmware side reads them via SdFat.

## Block layer

Reads and writes are served through the wear-levelling layer (`wl_read`,
`wl_erase_range` + `wl_write`) on the partition mounted at `startSession()` —
never raw `esp_partition` bytes, which would bypass the WL sector remap. The
MSC block size is the WL sector size (4096 with the prebuilt Arduino libs).
That equals the prebuilt `CFG_TUD_MSC_BUFSIZE`, so every host transfer is one
whole sector; a read-modify-write fallback covers partial-sector writes
defensively. If a legacy host that rejects 4096-byte-block disks ever
matters, a 512-byte 8:1 shim inside the two callbacks is the contained fix.

## Recovery property

Because MSC is block-level, a corrupted FAT volume (e.g. after a mid-write
unplug) can still be **repaired or backed up from the PC side** even when the
firmware's own mount fails. Consumers can build their "storage error" path
around this: leave USB transfer available and offer an explicit,
user-confirmed reformat — never format automatically over user data.
