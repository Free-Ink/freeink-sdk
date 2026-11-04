#pragma once

// UC8253 panel driver — Murphy M3 (CrowPanel 3.7", 240x416 B/W, ESP32-S3).
// Ported from the community-sdk feat-support-for-m3 implementation.
//
// Distinct from the X3 UC8253 driver: the controller is 240x416 but the device is
// held portrait with controller RAM rotated 90° relative to the UI, so the facade
// owns a 416x240 framebuffer (BoardProfile.displayWidth/Height) and this driver
// rotates each plane when writing to the UC8253. OEM LUTs are loaded per refresh.
//
// Refresh: the controller is hardware-reset and re-initialised before every
// refresh (manufacturer guidance) so stale LUT/RAM state can't leave pixels
// half-latched. A full (GC) refresh then writes the new frame to BOTH planes so
// only WW/BB fire and every pixel is fully driven to target — clean. A FAST (DU)
// refresh is differential: previous frame -> DTM1 (old), new frame -> DTM2 (new),
// so unchanged pixels take WW/BB and changed pixels take the quick BW/WB transition
// kicks. FAST ghosts over time, and the host decides when to ask for a full refresh; a config can
// set ghostClearInterval to have the driver promote one instead. Without a previous frame (single-buffer
// builds) the fast path falls back to both-planes-new (WW/BB only).
//
// Gray, with -DFREEINK_MURPHY_GRAY: the host draws gray pixels black in the B/W
// page, then one short nudge lifts them (see lut/Uc8253MurphyLuts.h). A later FAST
// would rest any such pixel the planes call unchanged and leave it gray, so the
// driver remembers them and marks them as changed in the old plane, which gives
// them the full drive. That costs no extra refresh.
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
  uint8_t ghostClearInterval;  // promote FAST -> full every N refreshes, 0 for never
  Uc8253MurphyLutBank gray = {};  // overlay nudge; a board without one shows no gray
};

const Uc8253MurphyConfig& uc8253MurphyDefaultConfig();

class Uc8253MurphyDriver : public PanelDriver {
 public:
  const char* controllerName() const override { return "UC8253"; }
  explicit Uc8253MurphyDriver(const Uc8253MurphyConfig& cfg = uc8253MurphyDefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }  // UC8253 BUSY low
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay || !_cfg.gray.vcom) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, false, false, false};
  }
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  // Without a gray bank this does nothing. A host still running a gray pass arrives here with a
  // gray PLANE in `fb`, and the default pushes that as a B/W page, which reads as a near-black
  // screen. The B/W page before the pass is already on the panel, so it stays there.
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;

 private:
  void initController(EpdBus& bus);
  void loadLut(EpdBus& bus, const Uc8253MurphyLutBank& bank);
  void writePlane(EpdBus& bus, uint8_t command, const uint8_t* fb);  // rotates 416x240 fb -> 240x416 RAM
  void writeOldPlane(EpdBus& bus, const uint8_t* old, const uint8_t* fb);  // `old`, gray pixels marked changed
  void triggerRefresh(EpdBus& bus, bool turnOff);
  uint8_t* grayPlane(uint8_t*& plane);

  const Uc8253MurphyConfig& _cfg;

  uint16_t _fbW;   // framebuffer width  (416)
  uint16_t _fbH;   // framebuffer height (240)
  uint16_t _fbWb;  // framebuffer width bytes (52)

  bool _isScreenOn = false;
  uint8_t _fastRefreshCount = 0;

  // A plane arrives in the write framebuffer, which the next plane overwrites, and every refresh
  // starts with a reset, so both wait here for displayGray(). Allocated on the first gray pass.
  uint8_t* _lsb = nullptr;
  uint8_t* _msb = nullptr;
  uint8_t* _gray = nullptr;  // which pixels the last nudge left gray
  bool _lsbStaged = false;
  bool _msbStaged = false;
  // The glass holds gray that the B/W planes do not describe. A second nudge would lift it further,
  // and the next display() has to drive it away.
  bool _grayOnGlass = false;
  // One refresh has run since the last reset and the controller is still up, so a nudge can skip
  // the reset, and DTM1 and DTM2 have traded roles. See displayGray().
  bool _rolesTraded = false;
};

PanelDriver& uc8253MurphyDriver();

}  // namespace freeink
