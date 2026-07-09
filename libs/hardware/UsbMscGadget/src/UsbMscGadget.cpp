#include "UsbMscGadget.h"

#include <Arduino.h>
#include <soc/soc_caps.h>

freeink::UsbMscGadget freeink::UsbMscGadget::instance;

#if FREEINK_CAP_USB_MSC && SOC_USB_OTG_SUPPORTED && !ARDUINO_USB_MODE

#include <USB.h>
#include <USBMSC.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <wear_levelling.h>

// TinyUSB device-mounted query (declared here to avoid dragging tusb.h into
// non-TinyUSB translation units via any shared header).
extern "C" bool tud_mounted(void);

namespace {

using Event = freeink::UsbMscGadget::Event;

// Global USBMSC: its constructor registers the MSC interface with the TinyUSB
// descriptor at static-init time — this must happen before app_main()'s
// automatic USB.begin() (which runs before setup() whenever
// ARDUINO_USB_CDC_ON_BOOT=1) or the LUN would miss the descriptor entirely.
USBMSC s_msc;

QueueHandle_t s_events = nullptr;
wl_handle_t s_wl = WL_INVALID_HANDLE;
volatile bool s_session = false;
volatile bool s_hostConnected = false;
size_t s_sectorSize = 0;

// RMW bounce buffer for the defensive partial-sector write path; sized to the
// prebuilt CONFIG_WL_SECTOR_SIZE (startSession rejects larger sectors).
constexpr size_t kMaxSectorSize = 4096;
uint8_t s_scratch[kMaxSectorSize];

void postEvent(Event ev) {
  if (!s_events) return;
  uint8_t v = static_cast<uint8_t>(ev);
  xQueueSend(s_events, &v, 0);
}

// Runs on the esp_event task.
void onUsbEvent(void*, esp_event_base_t, int32_t id, void*) {
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
      s_hostConnected = true;
      postEvent(Event::HostConnected);
      break;
    // RESUME re-raises HostConnected so a host waking from sleep re-engages
    // the consumer's transfer mode without a replug (idempotent for consumers
    // already in a session).
    case ARDUINO_USB_RESUME_EVENT:
      s_hostConnected = true;
      postEvent(Event::HostConnected);
      break;
    case ARDUINO_USB_STOPPED_EVENT:
    case ARDUINO_USB_SUSPEND_EVENT:
      s_hostConnected = false;
      postEvent(Event::HostDisconnected);
      break;
    default:
      break;
  }
}

// MSC callbacks run on the TinyUSB device task. s_wl is invalidated before the
// unmount in endSession(), so a straggling transfer fails cleanly instead of
// touching a freed handle.

int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (s_wl == WL_INVALID_HANDLE) return -1;
  if (wl_read(s_wl, static_cast<size_t>(lba) * s_sectorSize + offset, buffer, bufsize) != ESP_OK) return -1;
  return static_cast<int32_t>(bufsize);
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (s_wl == WL_INVALID_HANDLE) return -1;
  const size_t ss = s_sectorSize;

  // Fast path: whole-sector writes. The prebuilt CFG_TUD_MSC_BUFSIZE equals
  // the WL sector size, so in practice every transfer lands here.
  if (offset == 0 && bufsize >= ss && bufsize % ss == 0) {
    const size_t addr = static_cast<size_t>(lba) * ss;
    if (wl_erase_range(s_wl, addr, bufsize) != ESP_OK) return -1;
    if (wl_write(s_wl, addr, buffer, bufsize) != ESP_OK) return -1;
    return static_cast<int32_t>(bufsize);
  }

  // Defensive read-modify-write for partial-sector writes.
  size_t addr = static_cast<size_t>(lba) * ss + offset;
  uint32_t remaining = bufsize;
  const uint8_t* src = buffer;
  while (remaining) {
    const size_t sectorAddr = (addr / ss) * ss;
    const size_t within = addr - sectorAddr;
    uint32_t chunk = static_cast<uint32_t>(ss - within);
    if (chunk > remaining) chunk = remaining;
    if (wl_read(s_wl, sectorAddr, s_scratch, ss) != ESP_OK) return -1;
    memcpy(s_scratch + within, src, chunk);
    if (wl_erase_range(s_wl, sectorAddr, ss) != ESP_OK) return -1;
    if (wl_write(s_wl, sectorAddr, s_scratch, ss) != ESP_OK) return -1;
    addr += chunk;
    src += chunk;
    remaining -= chunk;
  }
  return static_cast<int32_t>(bufsize);
}

bool onStartStop(uint8_t, bool start, bool load_eject) {
  if (load_eject && !start) postEvent(Event::MediaEjected);
  return true;
}

}  // namespace

namespace freeink {

bool UsbMscGadget::begin(const char* product, const char* vendor) {
  if (!s_events) s_events = xQueueCreate(8, sizeof(uint8_t));
  if (!s_events) return false;

  s_msc.vendorID(vendor);
  s_msc.productID(product);
  s_msc.productRevision("1.0");
  s_msc.onStartStop(onStartStop);
  s_msc.onRead(onRead);
  s_msc.onWrite(onWrite);
  s_msc.mediaPresent(false);

  USB.onEvent(onUsbEvent);
  // Normally a no-op: ARDUINO_USB_CDC_ON_BOOT=1 already started the stack in
  // app_main(). Covers CDC_ON_BOOT=0 builds.
  const bool ok = USB.begin();

  // A host that enumerated us before this ran (plugged in during boot) fired
  // STARTED before the handler registration — synthesize the event.
  if (tud_mounted()) {
    s_hostConnected = true;
    postEvent(Event::HostConnected);
  }
  return ok;
}

bool UsbMscGadget::startSession(const char* partitionLabel) {
  if (s_session) return true;
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, partitionLabel);
  if (!part) return false;

  wl_handle_t wl = WL_INVALID_HANDLE;
  if (wl_mount(part, &wl) != ESP_OK) return false;

  const size_t sectorSize = wl_sector_size(wl);
  const size_t sectors = sectorSize ? wl_size(wl) / sectorSize : 0;
  if (!sectorSize || sectorSize > kMaxSectorSize || !sectors) {
    wl_unmount(wl);
    return false;
  }

  s_sectorSize = sectorSize;
  s_wl = wl;
  // USBMSC::begin() just publishes LUN geometry (re-callable); the interface
  // itself was registered by s_msc's constructor at static-init time.
  s_msc.begin(static_cast<uint32_t>(sectors), static_cast<uint16_t>(sectorSize));
  s_msc.isWritable(true);
  s_msc.mediaPresent(true);
  s_session = true;
  return true;
}

void UsbMscGadget::endSession() {
  if (!s_session) return;
  // Invalidate the handle before unmounting so an in-flight MSC callback
  // fails cleanly; give the TinyUSB task a beat to drain.
  const wl_handle_t wl = s_wl;
  s_wl = WL_INVALID_HANDLE;
  s_msc.mediaPresent(false);
  vTaskDelay(pdMS_TO_TICKS(20));
  wl_unmount(wl);
  s_session = false;
}

bool UsbMscGadget::sessionActive() const { return s_session; }

bool UsbMscGadget::hostConnected() const { return s_hostConnected; }

bool UsbMscGadget::popEvent(Event& out) {
  if (!s_events) return false;
  uint8_t v = 0;
  if (xQueueReceive(s_events, &v, 0) != pdTRUE) return false;
  out = static_cast<Event>(v);
  return true;
}

}  // namespace freeink

#else  // ---- stubs: no TinyUSB/OTG in this build --------------------------------

namespace freeink {

bool UsbMscGadget::begin(const char*, const char*) { return false; }
bool UsbMscGadget::startSession(const char*) { return false; }
void UsbMscGadget::endSession() {}
bool UsbMscGadget::sessionActive() const { return false; }
bool UsbMscGadget::hostConnected() const { return false; }
bool UsbMscGadget::popEvent(Event&) { return false; }

}  // namespace freeink

#endif
