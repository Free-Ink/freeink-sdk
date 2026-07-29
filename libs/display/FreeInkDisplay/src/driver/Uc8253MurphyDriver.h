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
// half-latched. A full (GC) refresh runs the OEM mode-0 init + DEFAULT bank,
// writes the new frame to BOTH planes so only WW/BB fire (every pixel fully
// driven to target), and triggers via 0x12 — clean.
//
// FAST is the OEM's alternate/partial package (FREEINK_MURPHY_OEM_PARTIAL,
// default 1): the data-driven init variant (0x82=0x07, 0x50=0xD7, pointer-fed
// 0x00/0x01/0x06/0x61 payloads), the ALT LUT bank (short single-kick
// waveforms), and the 0x17/0xA5 auto refresh trigger — Ghidra FUN_420389ec
// else-branch / FUN_42038b60 / FUN_42038f74. It is differential: previous
// frame -> DTM1 (old), new frame -> DTM2 (new), so unchanged pixels take the
// gentle WW/BB kicks and changed pixels the BW/WB transitions. Without a
// previous frame (facade passes prev=nullptr on single-buffer builds) it falls
// back to both-planes-new. The driver still promotes a fast refresh to a full
// one every ghostClearInterval refreshes since partial kicks ghost over time.
// Build with -DFREEINK_MURPHY_OEM_PARTIAL=0 to fall back to the old synthetic
// destination-drive FAST bank under the mode-0 init + 0x12 trigger.
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
  Uc8253MurphyLutBank fast;    // synthetic DU waveforms (used only when FREEINK_MURPHY_OEM_PARTIAL=0)
  Uc8253MurphyLutLens lens;    // per-register LUT byte counts for def/fast
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

  // B/W only: no gray LUT bank, no plane ingest (grayscale AA was tried on this
  // panel and abandoned — asymmetric VSH/VSL rails, see the Murphy repo display
  // findings). The PanelDriver default would refresh the renderer's gray PLANE
  // as if it were a B/W frame — a near-solid-black page — so it must be a no-op
  // here. supportsGrayscale() lets consumers skip building the planes at all.
  void displayGray(EpdBus&, const uint8_t*, bool, const unsigned char*, bool) override {}
  bool supportsGrayscale() const override { return false; }

 private:
  void initController(EpdBus& bus);
  void initControllerAlt(EpdBus& bus);  // OEM data-driven init variant (partial path)
  void loadLut(EpdBus& bus, const Uc8253MurphyLutBank& bank, const Uc8253MurphyLutLens& lens);
  void writePlane(EpdBus& bus, uint8_t command, const uint8_t* fb);  // rotates 416x240 fb -> 240x416 RAM
  void fillPlane(EpdBus& bus, uint8_t command, uint8_t fillByte);
  void triggerRefresh(EpdBus& bus, bool turnOff);
  void triggerRefreshAlt(EpdBus& bus);  // OEM 0x17/0xA5 auto sequence (partial path)

  const Uc8253MurphyConfig& _cfg;

  uint16_t _fbW;   // framebuffer width  (416)
  uint16_t _fbH;   // framebuffer height (240)
  uint16_t _fbWb;  // framebuffer width bytes (52)

  bool _isScreenOn = false;
  uint8_t _fastRefreshCount = 0;
};

PanelDriver& uc8253MurphyDriver();

}  // namespace freeink
