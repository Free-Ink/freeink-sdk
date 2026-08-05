#include "EpdBus.h"

#include <driver/gpio.h>
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
    : _bus(other._bus), _pmLock(std::move(other._pmLock)), _active(other._active) {
  other._bus = nullptr;
  other._active = false;
}

EpdBus::Transaction& EpdBus::Transaction::operator=(Transaction&& other) noexcept {
  if (this != &other) {
    end();
    _bus = other._bus;
    _pmLock = std::move(other._pmLock);
    _active = other._active;
    other._bus = nullptr;
    other._active = false;
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
}

uint8_t EpdBus::Transaction::transfer(uint8_t d) { return SPI.transfer(d); }

void EpdBus::Transaction::cmd(uint8_t c) {
  digitalWrite(_bus->_pins.dc, LOW);
  SPI.transfer(c);
  digitalWrite(_bus->_pins.dc, HIGH);
}

void EpdBus::Transaction::data(uint8_t d) {
  digitalWrite(_bus->_pins.dc, HIGH);
  SPI.transfer(d);
}

void EpdBus::Transaction::writeBytes(const uint8_t* d, uint16_t len) {
  digitalWrite(_bus->_pins.dc, HIGH);
  SPI.writeBytes(d, len);
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
  }

  {
    // SPI.begin() configures the peripheral/dividers from the current APB clock;
    // keep APB fixed here so the setup matches the requested display SPI rate.
    SpiApbLockGuard spiPmLock(*this);
    SPI.begin(pins.sclk, spiMiso, pins.mosi, pins.cs);
  }

  pinMode(pins.cs, OUTPUT);
  pinMode(pins.dc, OUTPUT);
  // Release any deep-sleep hold on RST (powerDownRailsForSleep() holds it HIGH so
  // the UC8179 stays in DSLP); the hold survives the wake reset and would make the
  // reset pulse below bounce off the latch.
  gpio_hold_dis(static_cast<gpio_num_t>(pins.rst));
  pinMode(pins.rst, OUTPUT);
  pinMode(pins.busy, busy == BusyPolarity::ActiveLow ? INPUT_PULLUP : INPUT);
  if (_coCs >= 0) {
    pinMode(_coCs, OUTPUT);
    digitalWrite(_coCs, HIGH);
  }
  digitalWrite(pins.cs, HIGH);
  digitalWrite(pins.dc, HIGH);
}

void EpdBus::reset(uint16_t extraSettleMs) {
  NoLightSleepLockGuard noLightSleepLock(*this);
  digitalWrite(_pins.rst, HIGH);
  delay(20);
  digitalWrite(_pins.rst, LOW);
  delay(2);
  digitalWrite(_pins.rst, HIGH);
  delay(20);
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
  } else if (p == BusyPolarity::ActiveLow) {
    bool busy = digitalRead(_pins.busy) == LOW;
    if (!busy) {
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
    waitBusy(tag);
    return;
  }
  // ISR-driven completion wait: sleep the task on a semaphore and wake on the
  // exact BUSY completion edge, instead of polling every 1 ms. Falls back to
  // polling if the semaphore could not be created.
  if (!s_epdRefreshDone) {
    waitBusy(tag);
    return;
  }

  // Levels/edge by polarity. X4 (ActiveHigh): working HIGH, done on the HIGH->LOW
  // (FALLING) edge. X3 (X3TwoPhase) / ActiveLow: working LOW, done on the LOW->HIGH
  // (RISING) edge.
  const bool activeHigh = (_busy == BusyPolarity::ActiveHigh);
  const int doneEdge = activeHigh ? FALLING : RISING;
  const int doneLevel = activeHigh ? LOW : HIGH;
  const int workingLevel = activeHigh ? HIGH : LOW;
  const unsigned long start = millis();

  // Confirm the waveform is actually running (BUSY at the working level) before
  // arming, so the already-done fast path below can't mistake the pre-start idle
  // level for completion. Bounded poll: if BUSY never shows the working level the
  // refresh was a no-op or already finished, and the fast path handles it. This
  // is a no-op for X3 (displayStart already drove BUSY to LOW) and ~instant for
  // X4 (SSD1677 asserts BUSY within microseconds of MASTER_ACTIVATION).
  {
    const unsigned long c0 = millis();
    while (digitalRead(_pins.busy) != workingLevel && millis() - c0 < 20) delay(1);
  }

  xSemaphoreTake(s_epdRefreshDone, 0);  // drain any stale token
  attachInterrupt(digitalPinToInterrupt(_pins.busy), epdBusyIsr, doneEdge);

  // Fast path: the waveform already finished (edge passed before we armed, or a
  // no-op refresh) — BUSY sits at the done level. Nothing to wait for. Safe
  // against the arm/edge race: the binary semaphore latches a give from the ISR,
  // so a take below returns immediately if the edge fired just after arming.
  if (digitalRead(_pins.busy) == doneLevel) {
    detachInterrupt(digitalPinToInterrupt(_pins.busy));
    xSemaphoreTake(s_epdRefreshDone, 0);
    return;
  }

  // Long sleep — fire the power hooks (if any) around it, matching the poll path.
  const bool hook = (_busyWaitBeginHook != nullptr);
  if (hook) _busyWaitBeginHook();
  xSemaphoreTake(s_epdRefreshDone, pdMS_TO_TICKS(30000));
  if (hook && _busyWaitEndHook != nullptr) _busyWaitEndHook();

  detachInterrupt(digitalPinToInterrupt(_pins.busy));
  if (tag && Serial) {
    Serial.printf("[%lu]   Wait complete: %s (%lu ms)\n", millis(), tag, millis() - start);
  }
}

void EpdBus::sendPlaneFlipped(uint8_t ramCmd, const uint8_t* plane, uint16_t height, uint16_t widthBytes) {
  cmd(ramCmd);  // own CS pulse
  auto txn = beginTxn();  // single CS-low burst for the whole plane
  for (int y = static_cast<int>(height) - 1; y >= 0; y--) {
    txn.writeBytes(plane + static_cast<uint32_t>(y) * widthBytes, widthBytes);
  }
}

void EpdBus::fillPlane(uint8_t ramCmd, uint8_t fillByte, uint16_t height, uint16_t widthBytes) {
  // Reusable constant-fill chunk; wide rows are written in multiple bursts so a
  // widthBytes larger than the chunk is streamed in full (no silent truncation).
  uint8_t chunk[128];
  memset(chunk, fillByte, sizeof(chunk));
  cmd(ramCmd);
  auto txn = beginTxn();
  for (uint16_t y = 0; y < height; y++) {
    uint16_t remaining = widthBytes;
    while (remaining) {
      const uint16_t n = remaining < sizeof(chunk) ? remaining : static_cast<uint16_t>(sizeof(chunk));
      txn.writeBytes(chunk, n);
      remaining = static_cast<uint16_t>(remaining - n);
    }
  }
}

}  // namespace freeink
