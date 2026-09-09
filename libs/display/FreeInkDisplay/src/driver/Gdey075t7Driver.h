#pragma once

#include "PanelDriver.h"

namespace freeink {

class Gdey075t7Driver : public PanelDriver {
 public:
  Gdey075t7Driver();

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::UcIdleHigh; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  bool supportsGrayscale() const override { return false; }
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override {}

 private:
  void initDisplay(EpdBus& bus);
  void powerOn(EpdBus& bus);
  void powerOff(EpdBus& bus);

  const uint16_t _w;
  const uint16_t _h;
  const uint16_t _wb;
  const uint32_t _bufferSize;
  bool _initialized = false;
  bool _screenOn = false;
  bool _firstRefresh = true;
};

PanelDriver& gdey075t7Driver();

}  // namespace freeink
