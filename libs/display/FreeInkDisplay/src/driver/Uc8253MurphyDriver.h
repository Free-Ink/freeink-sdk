#pragma once

// UC8253 panel driver — Murphy M3 (CrowPanel 3.7", 240x416 B/W, ESP32-S3).
//
// A scaffold. Distinct from the X3 UC8253 driver: different geometry, a 90°
// rotation between logical (240x416) and controller (416x240) coordinates, OEM
// LUTs loaded per refresh, a longer reset, and companion CHSC6x touch + PWM
// frontlight (handled by InputManager / FrontlightManager, not here). It supplies
// geometry, bus parameters, and the singleton so the Murphy build links; it does
// not implement the refresh path.
//
// Selection: only linked when -DFREEINK_DRIVER_UC8253_MURPHY (Murphy board env).

#include "PanelDriver.h"

namespace freeink {

class Uc8253MurphyDriver : public PanelDriver {
 public:
  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::X3TwoPhase; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
};

PanelDriver& uc8253MurphyDriver();

}  // namespace freeink
