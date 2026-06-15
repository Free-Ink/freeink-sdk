#pragma once

// UC8253 panel driver — Murphy M3 (CrowPanel 3.7", 240x416 B/W, ESP32-S3).
// Ported from the community-sdk feat-support-for-m3 implementation.
//
// Distinct from the X3 UC8253 driver: the controller is 240x416 but the device is
// held portrait with controller RAM rotated 90° relative to the UI, so the facade
// owns a 416x240 framebuffer (BoardProfile.displayWidth/Height) and this driver
// rotates each plane when writing to the UC8253. OEM LUTs are loaded per refresh.
//
// Refresh: differential. The previous frame goes to DTM1 (old) and the new frame
// to DTM2 (new), so the UC8253 picks a per-pixel waveform — unchanged pixels take
// WW/BB, changed pixels take the manufacturer's BW/WB transition waveforms. The
// DEFAULT (GC) LUTs do a ghost-clearing multi-level drive; the FAST (DU) LUTs do a
// quick single-direction drive, and the driver promotes a fast refresh to a full
// one every ghostClearInterval refreshes. When no previous frame is available
// (single-buffer builds) the new frame is written to both planes (WW/BB only).
//
// Selection: linked only when -DFREEINK_DRIVER_UC8253_MURPHY (Murphy board env).

#include "PanelDriver.h"

namespace freeink {

// One UC8253 waveform bank: the five LUT registers (0x20 VCOM .. 0x24 BB).
struct Uc8253MurphyLutBank {
  const uint8_t* vcom;  // 0x20
  const uint8_t* ww;    // 0x21
  const uint8_t* bw;    // 0x22
  const uint8_t* wb;    // 0x23
  const uint8_t* bb;    // 0x24
};

// Per-register LUT lengths. The UC8253 expects VCOM (0x20) and BW (0x22) at 56
// bytes and WW/WB/BB at 42 (manufacturer reference); each register is written at
// its own length so the controller latches the full waveform.
struct Uc8253MurphyLutLens {
  uint8_t vcom;  // 0x20
  uint8_t ww;    // 0x21
  uint8_t bw;    // 0x22
  uint8_t wb;    // 0x23
  uint8_t bb;    // 0x24
};

struct Uc8253MurphyConfig {
  Uc8253MurphyLutBank def;     // GC (ghost-clearing) waveforms
  Uc8253MurphyLutBank fast;    // DU (quick) waveforms
  Uc8253MurphyLutLens lens;    // per-register LUT byte counts
  uint8_t ghostClearInterval;  // promote FAST -> full every N refreshes
};

const Uc8253MurphyConfig& uc8253MurphyDefaultConfig();

class Uc8253MurphyDriver : public PanelDriver {
 public:
  explicit Uc8253MurphyDriver(const Uc8253MurphyConfig& cfg = uc8253MurphyDefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }  // UC8253 BUSY low
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

 private:
  void initController(EpdBus& bus);
  void loadLut(EpdBus& bus, const Uc8253MurphyLutBank& bank);
  void writePlane(EpdBus& bus, uint8_t command, const uint8_t* fb);  // rotates 416x240 fb -> 240x416 RAM
  void fillPlane(EpdBus& bus, uint8_t command, uint8_t fillByte);
  void triggerRefresh(EpdBus& bus, bool turnOff);

  const Uc8253MurphyConfig& _cfg;

  uint16_t _fbW;   // framebuffer width  (416)
  uint16_t _fbH;   // framebuffer height (240)
  uint16_t _fbWb;  // framebuffer width bytes (52)

  bool _isScreenOn = false;
  uint8_t _fastRefreshCount = 0;
};

PanelDriver& uc8253MurphyDriver();

}  // namespace freeink
