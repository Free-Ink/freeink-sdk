#include "EpdBus.h"

#include <driver/gpio.h>
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

namespace freeink {

namespace {

#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
esp_pm_lock_handle_t epdSpiApbLock() {
  static esp_pm_lock_handle_t lock = nullptr;
  static bool attempted = false;
  if (!attempted) {
    attempted = true;
    if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "freeink-epd-spi", &lock) != ESP_OK) {
      lock = nullptr;
    }
  }
  return lock;
}

esp_pm_lock_handle_t epdNoLightSleepLock() {
  static esp_pm_lock_handle_t lock = nullptr;
  static bool attempted = false;
  if (!attempted) {
    attempted = true;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "freeink-epd-bus", &lock) != ESP_OK) {
      lock = nullptr;
    }
  }
  return lock;
}
#endif

class NoLightSleepLock {
 public:
  NoLightSleepLock() {
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    _lock = epdNoLightSleepLock();
    _acquired = _lock != nullptr && esp_pm_lock_acquire(_lock) == ESP_OK;
#endif
  }

  ~NoLightSleepLock() {
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    if (_acquired) esp_pm_lock_release(_lock);
#endif
  }

  NoLightSleepLock(const NoLightSleepLock&) = delete;
  NoLightSleepLock& operator=(const NoLightSleepLock&) = delete;

 private:
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  esp_pm_lock_handle_t _lock = nullptr;
#endif
  bool _acquired = false;
};

}  // namespace

EpdBus::SpiPmLock::SpiPmLock(bool acquireNow) {
  if (acquireNow) {
    acquire();
  }
}

EpdBus::SpiPmLock::~SpiPmLock() { release(); }

EpdBus::SpiPmLock::SpiPmLock(SpiPmLock&& other) noexcept {
  _acquired = other._acquired;
  other._acquired = false;
}

EpdBus::SpiPmLock& EpdBus::SpiPmLock::operator=(SpiPmLock&& other) noexcept {
  if (this != &other) {
    release();
    _acquired = other._acquired;
    other._acquired = false;
  }
  return *this;
}

void EpdBus::SpiPmLock::acquire() {
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  if (_acquired) {
    return;
  }
  esp_pm_lock_handle_t lock = epdSpiApbLock();
  _acquired = lock != nullptr && esp_pm_lock_acquire(lock) == ESP_OK;
#endif
}

void EpdBus::SpiPmLock::release() {
#if defined(ARDUINO) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  if (_acquired) {
    esp_pm_lock_release(epdSpiApbLock());
    _acquired = false;
  }
#endif
}

EpdBus::Transaction::Transaction(EpdBus& bus) : _bus(&bus), _pmLock(true), _active(true) {
  _bus->beginRawTransaction();
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
  _bus->endRawTransaction();
  _pmLock.release();
  _active = false;
}

void EpdBus::Transaction::cmd(uint8_t c) { _bus->rawCmd(c); }

void EpdBus::Transaction::data(uint8_t d) { _bus->rawData(d); }

void EpdBus::Transaction::writeBytes(const uint8_t* d, uint16_t len) { _bus->rawWriteBytes(d, len); }

void EpdBus::begin(const EpdPins& pins, uint32_t spiHz, BusyPolarity busy, int8_t spiMiso, int8_t coCs) {
  NoLightSleepLock noLightSleepLock;
  _pins = pins;
  _spiHz = spiHz;
  _busy = busy;
  _coCs = coCs;
  _spi = SPISettings(spiHz, MSBFIRST, SPI_MODE0);

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
    SpiPmLock spiPmLock(true);
    SPI.begin(pins.sclk, spiMiso, pins.mosi, pins.cs);
  }

  pinMode(pins.cs, OUTPUT);
  pinMode(pins.dc, OUTPUT);
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
  NoLightSleepLock noLightSleepLock;
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

void EpdBus::cmd(uint8_t c) {
  auto txn = transaction();
  txn.cmd(c);
}

void EpdBus::data(uint8_t d) {
  auto txn = transaction();
  txn.data(d);
}

void EpdBus::data(const uint8_t* d, uint16_t len) {
  auto txn = transaction();
  txn.writeBytes(d, len);
}

void EpdBus::cmdData(uint8_t c, const uint8_t* d, uint16_t len) {
  auto txn = transaction();
  txn.cmd(c);
  if (len > 0 && d != nullptr) {
    txn.writeBytes(d, len);
  }
}

void EpdBus::cmdData2(uint8_t c, uint8_t d0, uint8_t d1) {
  const uint8_t d[2] = {d0, d1};
  cmdData(c, d, 2);
}

void EpdBus::beginRawTransaction() {
  if (_coCs >= 0) {
    digitalWrite(_coCs, HIGH);
  }
  SPI.beginTransaction(_spi);
  digitalWrite(_pins.cs, LOW);
}

void EpdBus::endRawTransaction() {
  digitalWrite(_pins.cs, HIGH);
  SPI.endTransaction();
}

void EpdBus::rawCmd(uint8_t c) {
  digitalWrite(_pins.dc, LOW);
  SPI.transfer(c);
  digitalWrite(_pins.dc, HIGH);
}

void EpdBus::rawData(uint8_t d) {
  digitalWrite(_pins.dc, HIGH);
  SPI.transfer(d);
}

void EpdBus::rawWriteBytes(const uint8_t* d, uint16_t len) {
  digitalWrite(_pins.dc, HIGH);
  SPI.writeBytes(d, len);
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

void EpdBus::writeMirroredPlane(const uint8_t* plane, uint16_t height, uint16_t widthBytes, bool invert) {
  uint8_t row[128];
  if (widthBytes > sizeof(row)) {
    widthBytes = sizeof(row);
  }
  for (uint16_t y = 0; y < height; y++) {
    const uint16_t srcY = static_cast<uint16_t>(height - 1 - y);
    const uint8_t* src = plane + static_cast<uint32_t>(srcY) * widthBytes;
    for (uint16_t x = 0; x < widthBytes; x++) {
      row[x] = invert ? static_cast<uint8_t>(~src[x]) : src[x];
    }
    data(row, widthBytes);
  }
}

void EpdBus::sendPlaneFlipped(uint8_t ramCmd, const uint8_t* plane, uint16_t height, uint16_t widthBytes) {
  cmd(ramCmd);  // own CS pulse
  auto txn = transaction();  // single CS-low burst for the whole plane
  for (int y = static_cast<int>(height) - 1; y >= 0; y--) {
    txn.writeBytes(plane + static_cast<uint32_t>(y) * widthBytes, widthBytes);
  }
}

void EpdBus::fillPlane(uint8_t ramCmd, uint8_t fillByte, uint16_t height, uint16_t widthBytes) {
  uint8_t row[128];
  if (widthBytes > sizeof(row)) widthBytes = sizeof(row);
  memset(row, fillByte, widthBytes);
  cmd(ramCmd);
  auto txn = transaction();
  for (uint16_t y = 0; y < height; y++) {
    txn.writeBytes(row, widthBytes);
  }
}

}  // namespace freeink
