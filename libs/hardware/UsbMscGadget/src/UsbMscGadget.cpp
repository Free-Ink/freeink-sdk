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
volatile bool s_session = false;
volatile bool s_hostConnected = false;

// The block device served to the host (read != nullptr while a session is
// active). Sessions started by label wrap a wear-levelled flash partition in
// the thunks below; sessions started with a consumer BlockSource (SD cards)
// pass through untouched.
freeink::UsbMscGadget::BlockSource s_src = {};
wl_handle_t s_wl = WL_INVALID_HANDLE;  // set only for wl-backed sessions

// Bounce buffer for the defensive partial-sector read/write paths; bounds the
// supported sector size (startSession rejects larger).
constexpr size_t kMaxSectorSize = 4096;
uint8_t s_scratch[kMaxSectorSize];

// Wear-levelled flash partition as a BlockSource. wl writes need an explicit
// erase first (NOR flash); SD-style sources handle that internally.
bool wlSourceRead(void*, uint32_t sector, uint8_t* dst, size_t count) {
  if (s_wl == WL_INVALID_HANDLE) return false;
  return wl_read(s_wl, static_cast<size_t>(sector) * s_src.sectorSize, dst, count * s_src.sectorSize) == ESP_OK;
}

bool wlSourceWrite(void*, uint32_t sector, const uint8_t* src, size_t count) {
  if (s_wl == WL_INVALID_HANDLE) return false;
  const size_t addr = static_cast<size_t>(sector) * s_src.sectorSize;
  const size_t len = count * s_src.sectorSize;
  if (wl_erase_range(s_wl, addr, len) != ESP_OK) return false;
  return wl_write(s_wl, addr, src, len) == ESP_OK;
}

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

// MSC callbacks run on the TinyUSB device task. s_src (and s_wl) are
// invalidated before teardown in endSession(), so a straggling transfer fails
// cleanly instead of touching a released device.

int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (!s_src.read) return -1;
  const size_t ss = s_src.sectorSize;

  // Fast path: whole sectors. With the prebuilt CFG_TUD_MSC_BUFSIZE (4096 = a
  // WL sector = 8 SD sectors) every transfer lands here.
  if (offset == 0 && bufsize >= ss && bufsize % ss == 0) {
    if (!s_src.read(s_src.ctx, lba, static_cast<uint8_t*>(buffer), bufsize / ss)) return -1;
    return static_cast<int32_t>(bufsize);
  }

  // Defensive partial-sector reads via the bounce buffer.
  size_t addr = static_cast<size_t>(lba) * ss + offset;
  uint32_t remaining = bufsize;
  uint8_t* dst = static_cast<uint8_t*>(buffer);
  while (remaining) {
    const uint32_t sector = static_cast<uint32_t>(addr / ss);
    const size_t within = addr - static_cast<size_t>(sector) * ss;
    uint32_t chunk = static_cast<uint32_t>(ss - within);
    if (chunk > remaining) chunk = remaining;
    if (!s_src.read(s_src.ctx, sector, s_scratch, 1)) return -1;
    memcpy(dst, s_scratch + within, chunk);
    addr += chunk;
    dst += chunk;
    remaining -= chunk;
  }
  return static_cast<int32_t>(bufsize);
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (!s_src.write) return -1;
  const size_t ss = s_src.sectorSize;

  // Fast path: whole sectors (see onRead).
  if (offset == 0 && bufsize >= ss && bufsize % ss == 0) {
    if (!s_src.write(s_src.ctx, lba, buffer, bufsize / ss)) return -1;
    return static_cast<int32_t>(bufsize);
  }

  // Defensive read-modify-write for partial-sector writes.
  size_t addr = static_cast<size_t>(lba) * ss + offset;
  uint32_t remaining = bufsize;
  const uint8_t* src = buffer;
  while (remaining) {
    const uint32_t sector = static_cast<uint32_t>(addr / ss);
    const size_t within = addr - static_cast<size_t>(sector) * ss;
    uint32_t chunk = static_cast<uint32_t>(ss - within);
    if (chunk > remaining) chunk = remaining;
    if (!s_src.read || !s_src.read(s_src.ctx, sector, s_scratch, 1)) return -1;
    memcpy(s_scratch + within, src, chunk);
    if (!s_src.write(s_src.ctx, sector, s_scratch, 1)) return -1;
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

  s_wl = wl;  // the thunks read this; cleared again if the start is rejected
  BlockSource source;
  source.sectorCount = static_cast<uint32_t>(sectors);
  source.sectorSize = static_cast<uint16_t>(sectorSize);
  source.read = wlSourceRead;
  source.write = wlSourceWrite;
  if (!startSession(source)) {
    s_wl = WL_INVALID_HANDLE;
    wl_unmount(wl);
    return false;
  }
  return true;
}

bool UsbMscGadget::startSession(const BlockSource& source) {
  if (s_session) return true;
  if (!source.read || !source.write || !source.sectorCount || !source.sectorSize ||
      source.sectorSize > kMaxSectorSize) {
    return false;
  }

  s_src = source;
  // USBMSC::begin() just publishes LUN geometry (re-callable); the interface
  // itself was registered by s_msc's constructor at static-init time.
  s_msc.begin(source.sectorCount, source.sectorSize);
  s_msc.isWritable(true);
  s_msc.mediaPresent(true);
  s_session = true;
  return true;
}

void UsbMscGadget::endSession() {
  if (!s_session) return;
  // Invalidate the source before teardown so an in-flight MSC callback fails
  // cleanly; give the TinyUSB task a beat to drain.
  const wl_handle_t wl = s_wl;
  s_wl = WL_INVALID_HANDLE;
  s_src = {};
  s_msc.mediaPresent(false);
  vTaskDelay(pdMS_TO_TICKS(20));
  if (wl != WL_INVALID_HANDLE) wl_unmount(wl);  // consumer sources release their own device
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
bool UsbMscGadget::startSession(const BlockSource&) { return false; }
void UsbMscGadget::endSession() {}
bool UsbMscGadget::sessionActive() const { return false; }
bool UsbMscGadget::hostConnected() const { return false; }
bool UsbMscGadget::popEvent(Event&) { return false; }

}  // namespace freeink

#endif
