#include "EpdBus.h"

#include <BoardConfig.h>
#include <driver/gpio.h>

#include <cstring>
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#if FREEINK_DEVICE_PAPERMONO
#include <PaperMonoBoard.h>
#endif
#if FREEINK_DEVICE_WS397
#include <Axp2101.h>
#endif

// Consumer escape hatch for boards whose EPD power/reset live behind glue the
// SDK doesn't know. Weak REFERENCES only — an SDK-internal implementation
// can't ride these (a weak ref doesn't force the archive member that defines
// it to link), which is why the Paper Mono path below is a direct call into
// its board-support header instead.
extern "C" void freeink_board_epd_power(bool enabled) __attribute__((weak));
extern "C" void freeink_board_epd_reset(bool high) __attribute__((weak));

namespace {
// EPD power/reset for boards without direct GPIOs: SDK board support first,
// then the consumer hooks.
void boardEpdPower(bool enabled) {
#if FREEINK_DEVICE_METALIO_EINK4
  if (BoardConfig::isMetalioEInk4()) {
    // Shared screen/SD rail must stay on until hardware power-off.
    if (enabled) freeink::metalio::ensureBooted();
    return;
  }
#endif
#if FREEINK_DEVICE_PAPERMONO
  freeink::papermono::setEpdPower(enabled);
#elif FREEINK_DEVICE_WS397
  freeink::axp2101::setEpdPower(enabled);  // ALDO3, not a GPIO
#else
  if (freeink_board_epd_power) freeink_board_epd_power(enabled);
#endif
}
bool boardEpdPowerAvailable() {
#if FREEINK_DEVICE_METALIO_EINK4
  if (BoardConfig::isMetalioEInk4()) return true;
#endif
#if FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_WS397
  return true;
#else
  return freeink_board_epd_power != nullptr;
#endif
}
void boardEpdReset(bool high) {
#if FREEINK_DEVICE_PAPERMONO
  freeink::papermono::setEpdReset(high);
#else
  if (freeink_board_epd_reset) freeink_board_epd_reset(high);
#endif
}
}  // namespace

namespace freeink {

// ── ISR-driven waveform-completion notification ──────────────────────────────
// A single binary semaphore, shared between the BUSY-pin GPIO ISR and
// waitRefreshComplete(). The ISR is attached only for the duration of one
// refresh wait (and only after the waveform is confirmed running), so it fires
// on the real completion edge, not on the idle->busy transition or SPI noise.
// File-static so the plain-C ISR can reach it; only one panel is ever active at
// a time, so a single instance is safe. DRAM_ATTR keeps it out of flash for the
// IRAM_ATTR ISR. Ported from the CrossPoint community-sdk EInkDisplay.
static DRAM_ATTR SemaphoreHandle_t s_epdRefreshDone = nullptr;

static void IRAM_ATTR epdBusyIsr() {
  if (!s_epdRefreshDone) return;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_epdRefreshDone, &woken);
  if (woken) portYIELD_FROM_ISR();
}

EpdBus::Transaction::Transaction(EpdBus& bus) : _bus(&bus), _pmLock(bus), _active(true) {
  SPI.beginTransaction(_bus->_spi);
}

EpdBus::Transaction::~Transaction() { end(); }

EpdBus::Transaction::Transaction(Transaction&& other) noexcept
    : _bus(other._bus), _pmLock(std::move(other._pmLock)), _active(other._active), _dcHigh(other._dcHigh) {
  other._bus = nullptr;
  other._active = false;
  other._dcHigh = false;
}

EpdBus::Transaction& EpdBus::Transaction::operator=(Transaction&& other) noexcept {
  if (this != &other) {
    end();
    _bus = other._bus;
    _pmLock = std::move(other._pmLock);
    _active = other._active;
    _dcHigh = other._dcHigh;
    other._bus = nullptr;
    other._active = false;
    other._dcHigh = false;
  }
  return *this;
}

void EpdBus::Transaction::end() {
  if (!_active || _bus == nullptr) {
    return;
  }
  digitalWrite(_bus->_pins.cs, HIGH);
  SPI.endTransaction();
  _pmLock.release();
  _active = false;
  _dcHigh = false;
}

uint8_t EpdBus::Transaction::transfer(uint8_t d) { return SPI.transfer(d); }

void EpdBus::Transaction::cmd(uint8_t c) {
  digitalWrite(_bus->_pins.dc, LOW);
  SPI.transfer(c);
  digitalWrite(_bus->_pins.dc, HIGH);
  _dcHigh = true;
}

void EpdBus::Transaction::data(uint8_t d) {
  if (!_dcHigh) {
    digitalWrite(_bus->_pins.dc, HIGH);
    _dcHigh = true;
  }
  SPI.transfer(d);
}

void EpdBus::Transaction::writeBytes(const uint8_t* d, uint16_t len) {
  if (!_dcHigh) {
    digitalWrite(_bus->_pins.dc, HIGH);
    _dcHigh = true;
  }
  SPI.writeBytes(d, len);
}

void EpdBus::Transaction::writeDataChunk(const uint8_t* d, size_t len) {
  if (!_active || _bus == nullptr || d == nullptr || len == 0) {
    return;
  }
  if (!_dcHigh) {
    digitalWrite(_bus->_pins.dc, HIGH);
    _dcHigh = true;
  }
  // 32 KB is an exact multiple of ESP32's 64-byte SPI FIFO (512 FIFOs), keeps
  // `d` 4-byte aligned across splits, and fits uint16_t host FakeSPI shims.
  static constexpr size_t MAX_SPI_WRITE_CHUNK = 32768u;
  while (len > 0) {
    const uint16_t chunk = static_cast<uint16_t>(len < MAX_SPI_WRITE_CHUNK ? len : MAX_SPI_WRITE_CHUNK);
    SPI.writeBytes(d, chunk);
    d += chunk;
    len -= chunk;
  }
}

EpdBus::~EpdBus() {
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  if (_spiApbLock != nullptr) {
    esp_pm_lock_delete(_spiApbLock);
    _spiApbLock = nullptr;
  }
  if (_noLightSleepLock != nullptr) {
    esp_pm_lock_delete(_noLightSleepLock);
    _noLightSleepLock = nullptr;
  }
#endif
}

void EpdBus::begin(const EpdPins& pins, uint32_t spiHz, BusyPolarity busy, int8_t spiMiso, int8_t coCs) {
  createPmLocks();
  NoLightSleepLockGuard noLightSleepLock(*this);
  _pins = pins;
  _spiHz = spiHz;
  _busy = busy;
  _coCs = coCs;
  _spi = SPISettings(spiHz, MSBFIRST, SPI_MODE0);

  // One-shot semaphore backing waitRefreshComplete()'s ISR wait (created once).
  if (!s_epdRefreshDone) s_epdRefreshDone = xSemaphoreCreateBinary();

  // Power the EPD rail first (boards that gate it, e.g. Sticky's EP_PWR_EN), so the
  // panel is alive before SPI bring-up and the reset pulse. No-op when unassigned.
  // gpio_hold_dis first: PowerManager::powerDownRailsForSleep() holds this pin LOW
  // for deep sleep, and the hold survives the wake reset — without releasing it,
  // the HIGH write silently bounces off the latch and the rail stays off.
  if (pins.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(pins.powerEnable));
    pinMode(pins.powerEnable, OUTPUT);
    digitalWrite(pins.powerEnable, HIGH);
    delay(100);
  } else if (boardEpdPowerAvailable()) {
    boardEpdPower(true);
    delay(100);
  }

  {
    // SPI.begin() configures the peripheral/dividers from the current APB clock;
    // keep APB fixed here so the setup matches the requested display SPI rate.
    SpiApbLockGuard spiPmLock(*this);
    SPI.begin(pins.sclk, spiMiso, pins.mosi, pins.cs);
  }

  pinMode(pins.cs, OUTPUT);
  pinMode(pins.dc, OUTPUT);
  // Release any deep-sleep hold on RST. powerDownRailsForSleep() holds it HIGH
  // while the panel rail remains powered, or LOW alongside a gated-off rail;
  // either hold survives the wake reset and would make the reset pulse below
  // bounce off the latch. Boards routing RST through an IO expander leave the
  // pin unassigned and reset via the freeink_board_epd_reset hook instead.
  if (pins.rst >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(pins.rst));
    pinMode(pins.rst, OUTPUT);
  }
  pinMode(pins.busy, (busy == BusyPolarity::ActiveLow || busy == BusyPolarity::ActiveLowReady) ? INPUT_PULLUP : INPUT);
  if (_coCs >= 0) {
    pinMode(_coCs, OUTPUT);
    digitalWrite(_coCs, HIGH);
  }
  digitalWrite(pins.cs, HIGH);
  digitalWrite(pins.dc, HIGH);
}

void EpdBus::reset(uint16_t extraSettleMs) {
  NoLightSleepLockGuard noLightSleepLock(*this);
  if (BoardConfig::isOnePage()) {
    // OnePage shares GPIO27 between EPD reset and the SD/MIC power rail.
    // Pulsing it low after Storage.begin() cuts power to the mounted SD card,
    // causing subsequent SD reads to fail. Keep the rail powered; SSD1677
    // performs CMD_SOFT_RESET immediately after.
    if (_pins.rst >= 0) {
      digitalWrite(_pins.rst, HIGH);
    }
    return;
  }
  const auto setReset = [this](bool high) {
    if (_pins.rst >= 0) {
      digitalWrite(_pins.rst, high ? HIGH : LOW);
    } else {
      boardEpdReset(high);
    }
  };
  setReset(true);
  delay(10);
  setReset(false);
  delay(10);
  setReset(true);
  delay(10);
  if (extraSettleMs) {
    delay(extraSettleMs);
  }
}

EpdBus::Transaction EpdBus::transaction() { return Transaction(*this); }

EpdBus::Transaction EpdBus::beginTxn() {
  if (_coCs >= 0) {
    digitalWrite(_coCs, HIGH);
  }
  auto txn = transaction();
  digitalWrite(_pins.cs, LOW);
  return txn;
}

EpdBus::Transaction EpdBus::beginDataTxn(uint8_t ramCmd) {
  cmd(ramCmd);  // own CS pulse, matching OEM UC8253 DTM framing
  auto txn = beginTxn();
  digitalWrite(_pins.dc, HIGH);
  txn._dcHigh = true;
  return txn;
}

size_t EpdBus::sendStream(uint8_t ramCmd, StreamChunkCallback nextChunk, void* ctx) {
  if (!nextChunk) {
    return 0;
  }
  auto txn = beginDataTxn(ramCmd);
  size_t totalBytes = 0;
  for (;;) {
    const uint8_t* chunk = nullptr;
    const size_t len = nextChunk(ctx, &chunk);
    if (len == 0 || !chunk) {
      break;
    }
    txn.writeDataChunk(chunk, len);
    totalBytes += len;
  }
  return totalBytes;
}

void EpdBus::cmd(uint8_t c) {
  auto txn = transaction();
  digitalWrite(_pins.dc, LOW);
  digitalWrite(_pins.cs, LOW);
  txn.transfer(c);
}

void EpdBus::data(uint8_t d) {
  auto txn = transaction();
  digitalWrite(_pins.dc, HIGH);
  digitalWrite(_pins.cs, LOW);
  txn.transfer(d);
}

void EpdBus::data(const uint8_t* d, uint16_t len) {
  auto txn = transaction();
  digitalWrite(_pins.dc, HIGH);
  digitalWrite(_pins.cs, LOW);
  SPI.writeBytes(d, len);
}

void EpdBus::cmdData(uint8_t c, const uint8_t* d, uint16_t len) {
  auto txn = transaction();
  digitalWrite(_pins.cs, LOW);
  digitalWrite(_pins.dc, LOW);
  txn.transfer(c);
  if (len > 0 && d != nullptr) {
    digitalWrite(_pins.dc, HIGH);
    SPI.writeBytes(d, len);
  }
}

void EpdBus::cmdData2(uint8_t c, uint8_t d0, uint8_t d1) {
  const uint8_t d[2] = {d0, d1};
  cmdData(c, d, 2);
}

void EpdBus::createPmLocks() {
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  if (_spiApbLock == nullptr) {
    if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "freeink-epd-spi", &_spiApbLock) != ESP_OK) {
      _spiApbLock = nullptr;
    }
  }
  if (_noLightSleepLock == nullptr) {
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "freeink-epd-bus", &_noLightSleepLock) != ESP_OK) {
      _noLightSleepLock = nullptr;
    }
  }
#endif
}

void EpdBus::waitBusy(const char* tag) { waitBusy(_busy, tag); }

void EpdBus::waitBusy(BusyPolarity p, const char* tag) {
  const unsigned long start = millis();
  // Both hooks engage lazily, only once the wait has proven long (see
  // setBusyWaitHooks). longWait gates the slice hook independently of the
  // begin hook's presence; hookFired guarantees the end hook is balanced.
  bool longWait = false;
  bool hookFired = false;
  bool x3SawLow = false;

  if (p == BusyPolarity::ActiveHigh) {
    while (digitalRead(_pins.busy) == HIGH) {
      busyIdle(longWait, HIGH, 1);
      if (!longWait && millis() - start > BUSY_WAIT_HOOK_THRESHOLD_MS) {
        longWait = true;
        if (_busyWaitBeginHook != nullptr) {
          hookFired = true;
          _busyWaitBeginHook();
        }
      }
      if (millis() - start > 30000) break;
    }
  } else if (p == BusyPolarity::ActiveLow || p == BusyPolarity::ActiveLowReady) {
    // Seeed's SSD2677 driver checks ready HIGH directly (Seeed_GFX2,
    // Driver_SSD2677::busyWait). Allow one tick for command propagation, but
    // don't add 100 ms when a command has already finished or never asserts BUSY.
    if (p == BusyPolarity::ActiveLowReady) delay(1);
    bool busy = digitalRead(_pins.busy) == LOW;
    if (!busy && p == BusyPolarity::ActiveLow) {
      while (millis() - start < 100) {
        if (digitalRead(_pins.busy) == LOW) {
          busy = true;
          break;
        }
        delay(1);
      }
    }
    if (busy) {
      do {
        busyIdle(longWait, LOW, 10);
        if (!longWait && millis() - start > BUSY_WAIT_HOOK_THRESHOLD_MS) {
          longWait = true;
          if (_busyWaitBeginHook != nullptr) {
            hookFired = true;
            _busyWaitBeginHook();
          }
        }
        if (millis() - start > 30000) break;
      } while (digitalRead(_pins.busy) == LOW);
    }
  } else if (p == BusyPolarity::UcIdleHigh) {
    // X4 Pro UC8179/UC8279 production behavior: allow one RTOS tick for the
    // command to take effect, then wait only for the documented idle level.
    // There is deliberately no fixed millisecond timeout on this controller
    // path; issuing the next command while BUSY_N is still LOW can make the UC
    // controller discard plane or LUT writes.
    delay(1);
    while (digitalRead(_pins.busy) == LOW) {
      busyIdle(longWait, LOW, 1);
      if (!longWait && millis() - start > BUSY_WAIT_HOOK_THRESHOLD_MS) {
        longWait = true;
        if (_busyWaitBeginHook != nullptr) {
          hookFired = true;
          _busyWaitBeginHook();
        }
      }
    }
  } else {  // X3TwoPhase: wait for the LOW edge, then wait back to HIGH
    while (digitalRead(_pins.busy) == HIGH) {
      delay(1);
      if (millis() - start > 1000) break;
    }
    if (digitalRead(_pins.busy) == LOW) {
      x3SawLow = true;
      while (digitalRead(_pins.busy) == LOW) {
        busyIdle(longWait, LOW, 1);
        if (!longWait && millis() - start > BUSY_WAIT_HOOK_THRESHOLD_MS) {
          longWait = true;
          if (_busyWaitBeginHook != nullptr) {
            hookFired = true;
            _busyWaitBeginHook();
          }
        }
        if (millis() - start > 30000) break;
      }
    }
  }

  if (hookFired && _busyWaitEndHook != nullptr) _busyWaitEndHook();
  if (p == BusyPolarity::X3TwoPhase && !x3SawLow) return;

  if (tag && Serial) {
    Serial.printf("[%lu]   Wait complete: %s (%lu ms)\n", millis(), tag, millis() - start);
  }
}

void EpdBus::waitRefreshComplete(const char* tag) {
  // The X4 Pro UC production wait is level-based, not edge-qualified. Keep the
  // same one-tick/idle-HIGH rule for refresh completion so a missed assertion
  // edge can never make the caller write RAM while the waveform is still busy.
  if (_busy == BusyPolarity::UcIdleHigh) {
    waitBusy(_busy, tag);
    return;
  }
  // Keep refresh assertion grace even when command waits use the faster
  // ready-level policy. Both polling fallbacks must still catch a late BUSY.
  const BusyPolarity refreshPolarity = _busy == BusyPolarity::ActiveLowReady ? BusyPolarity::ActiveLow : _busy;
  // A host that installed a busy-wait slice hook (e.g. CrossPoint light-sleeping
  // through the refresh) must keep the polling path: waitBusy() invokes the slice
  // hook on each idle step, while this ISR path sleeps the task on a semaphore and
  // never calls it. Bypassing the hook costs that host its power policy (~9% more
  // per refresh, measured ~29 mC vs ~26.5 mC on X3), and is a latent hazard: edge
  // interrupts do not fire during light sleep, so a completion edge taken while the
  // host is slept would be missed and the wait would stall to its 30 s timeout. The
  // slice hook already delivers GPIO-precise wake, so the ISR path buys these hosts
  // nothing — fall back to the hooked poll.
  if (_busyWaitSliceHook != nullptr) {
    // Refresh-completion context: BUSY assertion can trail MASTER_ACTIVATION
    // by a few microseconds, and the ActiveHigh polled path (unlike ActiveLow's
    // 100 ms grace loop, or the ISR path's 20 ms edge wait below) would fall
    // through immediately — returning mid-waveform, after which single-buffer
    // drivers rewrite controller RAM while the panel is still driving. Give
    // the poll the same bounded grace here, in the refresh-only context, so
    // waitBusy() itself (which also serves command waits where BUSY may never
    // assert) stays untouched for every other caller.
    if (_busy == BusyPolarity::ActiveHigh) {
      const unsigned long graceStart = millis();
      while (digitalRead(_pins.busy) != HIGH && millis() - graceStart < 20) {
        delayMicroseconds(200);
      }
    } else if (refreshPolarity == BusyPolarity::X3TwoPhase) {
      const unsigned long graceStart = millis();
      while (digitalRead(_pins.busy) == HIGH && millis() - graceStart < 20) {
        delayMicroseconds(200);
      }
      if (digitalRead(_pins.busy) == HIGH) {
        return;
      }
      waitBusy(BusyPolarity::ActiveLowReady, tag);
      return;
    }
    waitBusy(refreshPolarity, tag);
    return;
  }
  // ISR-driven completion wait: sleep the task on a semaphore and wake on the
  // exact BUSY completion edge, instead of polling every 1 ms. Falls back to
  // polling if the semaphore could not be created.
  if (!s_epdRefreshDone) {
    waitBusy(refreshPolarity, tag);
    return;
  }
  // Levels by polarity. CHANGE is armed so both the delayed BUSY assertion and
  // its completion are event-driven; the task never polls during either phase.
  const bool activeHigh = (_busy == BusyPolarity::ActiveHigh);
  const int doneLevel = activeHigh ? LOW : HIGH;
  const int workingLevel = activeHigh ? HIGH : LOW;
  const unsigned long start = millis();

  xSemaphoreTake(s_epdRefreshDone, 0);  // drain any stale token
  attachInterrupt(digitalPinToInterrupt(_pins.busy), epdBusyIsr, CHANGE);

  bool sawWorking = digitalRead(_pins.busy) == workingLevel;
  if (!sawWorking) {
    // BUSY assertion can trail MASTER_ACTIVATION by a few microseconds. Sleep
    // until an edge instead of delay(1) polling. No edge in 20 ms means the
    // command was a no-op or its entire pulse completed before we armed.
    if (xSemaphoreTake(s_epdRefreshDone, pdMS_TO_TICKS(20)) != pdTRUE) {
      detachInterrupt(digitalPinToInterrupt(_pins.busy));
      return;
    }
    sawWorking = digitalRead(_pins.busy) == workingLevel;
    if (!sawWorking && digitalRead(_pins.busy) == doneLevel) {
      detachInterrupt(digitalPinToInterrupt(_pins.busy));
      return;
    }
  }

  // Long sleep — fire the power hooks (if any) around it, matching the poll path.
  const bool hook = (_busyWaitBeginHook != nullptr);
  if (hook) _busyWaitBeginHook();
  while (digitalRead(_pins.busy) != doneLevel) {
    if (xSemaphoreTake(s_epdRefreshDone, pdMS_TO_TICKS(30000)) != pdTRUE) break;
  }
  if (hook && _busyWaitEndHook != nullptr) _busyWaitEndHook();

  detachInterrupt(digitalPinToInterrupt(_pins.busy));
  if (tag && Serial) {
    Serial.printf("[%lu]   Wait complete: %s (%lu ms)\n", millis(), tag, millis() - start);
  }
}

namespace {
// 256-byte (4 x 64-byte ESP32-C3 SPI FIFO) 4-byte-aligned bounce/fill chunks so
// esp32-hal-spi.c's spiWriteNL() issues aligned 32-bit word loads without partial-FIFO stalls.
constexpr size_t PLANE_BOUNCE_BYTES = 256u;
constexpr size_t FILL_CHUNK_BYTES = 256u;

void invertBytesInPlace(uint8_t* buf, const size_t len) {
  size_t i = 0;
  for (; i + sizeof(uint32_t) <= len; i += sizeof(uint32_t)) {
    uint32_t word;
    std::memcpy(&word, buf + i, sizeof(word));
    word = ~word;
    std::memcpy(buf + i, &word, sizeof(word));
  }
  for (; i < len; ++i) {
    buf[i] = static_cast<uint8_t>(~buf[i]);
  }
}
}  // namespace

void EpdBus::sendPlaneFlipped(uint8_t ramCmd, const uint8_t* plane, uint16_t height, uint16_t widthBytes) {
  auto txn = beginDataTxn(ramCmd);  // ramCmd pulse, then single CS-low data burst
  if (plane == nullptr || height == 0 || widthBytes == 0) {
    return;
  }
  alignas(4) uint8_t bounce[PLANE_BOUNCE_BYTES];
  const size_t wb = widthBytes;
  size_t buffered = 0;
  if (wb <= sizeof(bounce)) {
    const uint8_t* src = plane + static_cast<size_t>(height) * wb;
    for (size_t y = height; y > 0; --y) {
      src -= wb;
      const size_t space = sizeof(bounce) - buffered;
      if (wb <= space) {
        std::memcpy(bounce + buffered, src, wb);
        buffered += wb;
        if (buffered == sizeof(bounce)) {
          txn.writeDataChunk(bounce, sizeof(bounce));
          buffered = 0;
        }
      } else {
        std::memcpy(bounce + buffered, src, space);
        txn.writeDataChunk(bounce, sizeof(bounce));
        buffered = wb - space;
        std::memcpy(bounce, src + space, buffered);
      }
    }
    if (buffered > 0) {
      txn.writeDataChunk(bounce, buffered);
    }
    return;
  }
  for (size_t y = height; y > 0; --y) {
    const uint8_t* src = plane + (y - 1u) * wb;
    size_t rem = wb;
    while (rem > 0) {
      const size_t space = sizeof(bounce) - buffered;
      const size_t copyLen = rem < space ? rem : space;
      std::memcpy(bounce + buffered, src, copyLen);
      src += copyLen;
      rem -= copyLen;
      buffered += copyLen;
      if (buffered == sizeof(bounce)) {
        txn.writeDataChunk(bounce, sizeof(bounce));
        buffered = 0;
      }
    }
  }
  if (buffered > 0) {
    txn.writeDataChunk(bounce, buffered);
  }
}

void EpdBus::sendPlaneFlippedInverted(uint8_t ramCmd, const uint8_t* plane, uint16_t height, uint16_t widthBytes) {
  // Streams ~plane[i] row-reversed without a host-side copy of the inverted
  // frame (the C3 boards have no RAM to spare for one). Same single CS-low
  // burst contract as sendPlaneFlipped(), packed into 256-byte chunks.
  auto txn = beginDataTxn(ramCmd);
  if (plane == nullptr || height == 0 || widthBytes == 0) {
    return;
  }
  alignas(4) uint8_t bounce[PLANE_BOUNCE_BYTES];
  const size_t wb = widthBytes;
  size_t buffered = 0;
  if (wb <= sizeof(bounce)) {
    const uint8_t* src = plane + static_cast<size_t>(height) * wb;
    for (size_t y = height; y > 0; --y) {
      src -= wb;
      const size_t space = sizeof(bounce) - buffered;
      if (wb <= space) {
        std::memcpy(bounce + buffered, src, wb);
        buffered += wb;
        if (buffered == sizeof(bounce)) {
          invertBytesInPlace(bounce, sizeof(bounce));
          txn.writeDataChunk(bounce, sizeof(bounce));
          buffered = 0;
        }
      } else {
        std::memcpy(bounce + buffered, src, space);
        invertBytesInPlace(bounce, sizeof(bounce));
        txn.writeDataChunk(bounce, sizeof(bounce));
        buffered = wb - space;
        std::memcpy(bounce, src + space, buffered);
      }
    }
    if (buffered > 0) {
      invertBytesInPlace(bounce, buffered);
      txn.writeDataChunk(bounce, buffered);
    }
    return;
  }
  for (size_t y = height; y > 0; --y) {
    const uint8_t* src = plane + (y - 1u) * wb;
    size_t rem = wb;
    while (rem > 0) {
      const size_t space = sizeof(bounce) - buffered;
      const size_t copyLen = rem < space ? rem : space;
      std::memcpy(bounce + buffered, src, copyLen);
      src += copyLen;
      rem -= copyLen;
      buffered += copyLen;
      if (buffered == sizeof(bounce)) {
        invertBytesInPlace(bounce, sizeof(bounce));
        txn.writeDataChunk(bounce, sizeof(bounce));
        buffered = 0;
      }
    }
  }
  if (buffered > 0) {
    invertBytesInPlace(bounce, buffered);
    txn.writeDataChunk(bounce, buffered);
  }
}

void EpdBus::fillPlane(uint8_t ramCmd, uint8_t fillByte, uint16_t height, uint16_t widthBytes) {
  // Stream height * widthBytes constant bytes as 256-byte (4 x 64 B FIFO) bursts
  // inside one CS-low transaction; handles any widthBytes without truncation.
  auto txn = beginDataTxn(ramCmd);
  size_t remaining = static_cast<size_t>(height) * widthBytes;
  if (remaining == 0) {
    return;
  }
  alignas(4) uint8_t chunk[FILL_CHUNK_BYTES];
  std::memset(chunk, fillByte, sizeof(chunk));
  while (remaining > 0) {
    const size_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    txn.writeDataChunk(chunk, n);
    remaining -= n;
  }
}

}  // namespace freeink
