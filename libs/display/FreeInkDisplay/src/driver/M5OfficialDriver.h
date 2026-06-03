#pragma once

// M5Stack PaperColor — official-library display backend.
//
// An alternative to the fast hand-rolled Ed2208M5Driver: this wraps M5's own
// stack (M5Unified + M5GFX, panel EL040EF1 / Spectra 6, 400x600) for users who
// prefer the vendor-supported path. M5GFX owns the SPI/display hardware, so this
// driver reports usesExternalBus() == true and the facade leaves its EpdBus
// down. Reports the same 600x400 landscape geometry as the native driver so the
// app renders identically regardless of backend.
//
// Selection: only linked when -DFREEINK_M5_OFFICIAL=1 (which also adds the M5
// libraries to lib_deps — see platformio.sample.ini). Default M5 builds use the
// faster Ed2208M5Driver.

#include "PanelDriver.h"

namespace freeink {

class M5OfficialDriver : public PanelDriver {
 public:
  uint32_t spiHz() const override { return 0; }  // M5GFX owns the bus
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  bool usesExternalBus() const override { return true; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
};

PanelDriver& m5OfficialDriver();

}  // namespace freeink
