#pragma once

#include <Arduino.h>

#include "BoardT5S3Pins.h"

namespace BoardT5S3 {

// Holds the SDK's shared I2C lock (freeink::i2cBusMutex(), I2cBusLock.h) for its scope, so a
// caller outside this layer serialises with it and with the SDK's I2C helpers.
class ScopedI2CLock {
 public:
  ScopedI2CLock();
  ~ScopedI2CLock();

  ScopedI2CLock(const ScopedI2CLock&) = delete;
  ScopedI2CLock& operator=(const ScopedI2CLock&) = delete;

 private:
  bool locked_ = false;
};

void begin();
void beginI2C();
void prepareSdBus();
void disableGpsLora();
bool pca9535Present();
bool readPca9535Pin(uint8_t pin, bool* high);
bool writePca9535Pin(uint8_t pin, bool high);
bool setPca9535PinMode(uint8_t pin, uint8_t mode);
bool readButton();

}  // namespace BoardT5S3
