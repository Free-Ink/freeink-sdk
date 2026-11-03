#pragma once
// The main loop and the firmware's render task both use the I2C helpers (input polling there,
// the battery gauge here). Two transfers in flight at once fail with ESP_ERR_INVALID_STATE, so
// every helper holds this lock for one complete transfer. Recursive: helpers may nest.
//
// Only the Metalio enables it (see metalio::ensureBooted()); on every other board it does
// nothing until it has been tested there.
#if defined(ESP_PLATFORM) && FREEINK_DEVICE_METALIO_EINK4
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace freeink {

#if defined(ESP_PLATFORM) && FREEINK_DEVICE_METALIO_EINK4
inline bool& i2cBusLockEnabled() {
  static bool enabled = false;
  return enabled;
}

// Call before any second task uses I2C.
inline void enableI2cBusLock() { i2cBusLockEnabled() = true; }

inline SemaphoreHandle_t i2cBusMutex() {
  static StaticSemaphore_t storage;
  static const SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexStatic(&storage);
  return mutex;
}

class I2cBusLock {
 public:
  // held_ keeps take and give paired even if the lock is enabled while this guard is alive.
  I2cBusLock() : held_(i2cBusLockEnabled()) {
    if (held_) xSemaphoreTakeRecursive(i2cBusMutex(), portMAX_DELAY);
  }
  ~I2cBusLock() {
    if (held_) xSemaphoreGiveRecursive(i2cBusMutex());
  }
  I2cBusLock(const I2cBusLock&) = delete;
  I2cBusLock& operator=(const I2cBusLock&) = delete;

 private:
  const bool held_;
};
#else
inline void enableI2cBusLock() {}

// User-provided members keep guard variables "used".
class I2cBusLock {
 public:
  I2cBusLock() {}
  ~I2cBusLock() {}
};
#endif

}  // namespace freeink
