#include "EpdBus.h"

namespace freeink {

void EpdBus::begin(const EpdPins& pins, uint32_t spiHz, BusyPolarity busy, int8_t spiMiso, int8_t coCs) {
  _pins = pins;
  _spiHz = spiHz;
  _busy = busy;
  _coCs = coCs;
  _spi = SPISettings(spiHz, MSBFIRST, SPI_MODE0);

  // Power the EPD rail first (boards that gate it, e.g. Sticky's EP_PWR_EN), so the
  // panel is alive before SPI bring-up and the reset pulse. No-op when unassigned.
  if (pins.powerEnable >= 0) {
    pinMode(pins.powerEnable, OUTPUT);
    digitalWrite(pins.powerEnable, HIGH);
    delay(100);
  }

  SPI.begin(pins.sclk, spiMiso, pins.mosi, pins.cs);

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

void EpdBus::cmd(uint8_t c) {
  SPI.beginTransaction(_spi);
  digitalWrite(_pins.dc, LOW);
  digitalWrite(_pins.cs, LOW);
  SPI.transfer(c);
  digitalWrite(_pins.cs, HIGH);
  SPI.endTransaction();
}

void EpdBus::data(uint8_t d) {
  SPI.beginTransaction(_spi);
  digitalWrite(_pins.dc, HIGH);
  digitalWrite(_pins.cs, LOW);
  SPI.transfer(d);
  digitalWrite(_pins.cs, HIGH);
  SPI.endTransaction();
}

void EpdBus::data(const uint8_t* d, uint16_t len) {
  SPI.beginTransaction(_spi);
  digitalWrite(_pins.dc, HIGH);
  digitalWrite(_pins.cs, LOW);
  SPI.writeBytes(d, len);
  digitalWrite(_pins.cs, HIGH);
  SPI.endTransaction();
}

void EpdBus::cmdData(uint8_t c, const uint8_t* d, uint16_t len) {
  SPI.beginTransaction(_spi);
  digitalWrite(_pins.cs, LOW);
  digitalWrite(_pins.dc, LOW);
  SPI.transfer(c);
  if (len > 0 && d != nullptr) {
    digitalWrite(_pins.dc, HIGH);
    SPI.writeBytes(d, len);
  }
  digitalWrite(_pins.cs, HIGH);
  SPI.endTransaction();
}

void EpdBus::cmdData2(uint8_t c, uint8_t d0, uint8_t d1) {
  const uint8_t d[2] = {d0, d1};
  cmdData(c, d, 2);
}

void EpdBus::beginTxn() {
  if (_coCs >= 0) {
    digitalWrite(_coCs, HIGH);
  }
  SPI.beginTransaction(_spi);
  digitalWrite(_pins.cs, LOW);
}

void EpdBus::endTxn() {
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

  if (p == BusyPolarity::ActiveHigh) {
    while (digitalRead(_pins.busy) == HIGH) {
      delay(1);
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
        delay(10);
        if (millis() - start > 30000) break;
      } while (digitalRead(_pins.busy) == LOW);
    }
  } else {  // X3TwoPhase: wait for the LOW edge, then wait back to HIGH
    bool sawLow = false;
    while (digitalRead(_pins.busy) == HIGH) {
      delay(1);
      if (millis() - start > 1000) break;
    }
    if (digitalRead(_pins.busy) == LOW) {
      sawLow = true;
      while (digitalRead(_pins.busy) == LOW) {
        delay(1);
        if (millis() - start > 30000) break;
      }
    }
    if (!sawLow) return;
  }

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
  beginTxn();   // single CS-low burst for the whole plane
  for (int y = static_cast<int>(height) - 1; y >= 0; y--) {
    rawWriteBytes(plane + static_cast<uint32_t>(y) * widthBytes, widthBytes);
  }
  endTxn();
}

void EpdBus::fillPlane(uint8_t ramCmd, uint8_t fillByte, uint16_t height, uint16_t widthBytes) {
  uint8_t row[128];
  if (widthBytes > sizeof(row)) widthBytes = sizeof(row);
  memset(row, fillByte, widthBytes);
  cmd(ramCmd);
  beginTxn();
  for (uint16_t y = 0; y < height; y++) {
    rawWriteBytes(row, widthBytes);
  }
  endTxn();
}

}  // namespace freeink
