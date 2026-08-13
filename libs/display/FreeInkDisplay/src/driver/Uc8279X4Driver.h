#pragma once

// UC8279 panel driver — Xteink X4 Pro production runs that ship an UltraChip
// UC8279 (800x480) in place of the SSD1677. NOT the X3's UC8279d (792x528,
// Uc8279Driver) — this variant has its own init (PSR 0x37/0x4D, PLL 0x0E, PFS),
// a 1-byte CDI, a 120-gate offset on the 600-gate scan, and an external-LUT AA
// grayscale path with bitwise-INVERTED planes.
//
// Register sequences, waveform tables, and power ordering come from the Xteink
// X4 Pro 480x800 display hardware reference (vendor R&D doc). Identification:
// VER (0x70) byte2 LUT_VER = 0x02 or 0x68 (0x69 reserved — routed here too, but
// with no AA waveform of its own it uses the 0x68 table; built-in refreshes are
// identical). The boot probe stores that byte in
// BoardConfig::ACTIVE.displayControllerVariant. PENDING HARDWARE VALIDATION on
// a UC8279 X4 Pro unit — no such panel has been on the bench yet.
//
// Same KW differential paradigm as the UC8179 sibling: DTM1 (0x10) = OLD plane,
// DTM2 (0x13) = NEW plane, OTP waveforms for B/W (PSR REG=0 at refresh),
// external 5x49 LUTs for AA grayscale (REG=1). BUSY_N is low while busy;
// production waits one tick and then polls until it returns HIGH.

#include "PanelDriver.h"

namespace freeink {

struct Uc8279X4Config {
  // PSR (0x00) byte 0 as written at init and for the AA path (REG bit5=1,
  // external LUT). Built-in refreshes re-assert (psr0 & 0xDF) = REG=0 -> OTP.
  uint8_t psr0;
  // PSR (0x00) byte 1.
  uint8_t psr1;
  // PFS power-off sequence (cmd 0x03).
  uint8_t pfs;
  // PLL frame-rate (cmd 0x30) — unlike the UC8179, this variant's init programs it.
  uint8_t pll;
  // Gate-scan selection (cmd 0xE1).
  uint8_t gateScan;
  // CCSET cascade/output enable (cmd 0xE0), built-in refreshes only.
  uint8_t ccset;
  // TSSET forced temperature (cmd 0xE5) for a full refresh.
  uint8_t tsset;
  // TSSET (cmd 0xE5) for a fast/partial refresh.
  uint8_t tssetFast;
  // CDI (0x50) — SINGLE byte on this controller. First AA refresh after init
  // drives the border; later AA refreshes hold it.
  uint8_t cdiAaFirst;
  uint8_t cdiAaLater;
  // TRES (0x61) gate count: the panel is addressed 800x600 with 480 visible.
  uint16_t tresHeight;
  // First visible gate: the UC8279 scans 600 gates with the bonded 480 starting
  // at this offset; rows outside it are padded 0xFF (white) in every DTM write.
  uint16_t gateOffset;
};

const Uc8279X4Config& uc8279X4DefaultConfig();

class Uc8279X4Driver : public PanelDriver {
 public:
  explicit Uc8279X4Driver(const Uc8279X4Config& cfg = uc8279X4DefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::UcIdleHigh; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  bool supportsAsyncDisplay() const override { return true; }

  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;
  // Inverted (dark-background) content: fast refreshes rewrite the OLD plane
  // as the complement of the target so every pixel is re-driven toward its
  // target each update. See displayStart().
  void setBackgroundHint(bool darkBackground) override { _darkBackground = darkBackground; }

  // --- 4-level grayscale (anti-aliasing) ---
  // External-LUT path per the vendor reference: both planes bitwise-INVERTED
  // (plane0/LSB -> 0x10, plane1/MSB -> 0x13 — the reverse of the built-in 4-gray
  // order), 5x49 LUTs whose bytes depend on the LUT_VER variant (0x02 vs 0x68),
  // single-byte CDI (first/later), PSR rewritten before DRF, and the panel LEFT
  // POWERED between AA page refreshes (vendor production behavior).
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;

 private:
  void initController(EpdBus& bus);
  // Stream a framebuffer into a RAM plane: 0xFF padding for gates before the
  // visible offset, mirror-Y row reversal over the visible rows (same orientation
  // convention as the UC8179 sibling; mirror-X via the PSR SHL bit), then 0xFF
  // padding to the addressed gate count. `invert` bitwise-inverts the image rows
  // (AA planes only, per the vendor reference).
  void streamPlane(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb, bool invert = false);
  void powerOnIfNeeded(EpdBus& bus, const char* tag);

  const Uc8279X4Config& _cfg;

  uint16_t _w;      // visible width (800)
  uint16_t _h;      // visible height (480)
  uint16_t _wb;     // width in bytes (100)
  uint16_t _tresH;  // addressed gate count (600)
  uint32_t _bufferSize;

  bool _isScreenOn = false;
  bool _darkBackground = false;
  bool _needFullClear = true;
  bool _oldPlaneValid = false;
  // Set after every grayscale (AA) refresh. The AA overlay leaves gray edge
  // charge the plain B/W fast diff can't scrub (the B/W baseline records those
  // pixels as white), so it accumulates under rapid page turns → garble. Consumed
  // by the next B/W displayStart to RE-DRIVE every pixel to its target (DTM1 =
  // ~newframe), scrubbing the residue with a cheap DU (no GC flash) — the same
  // trick as _darkBackground, applied once after AA.
  bool _redriveAfterGray = false;
  // AA CDI select: first grayscale refresh after init sends cdiAaFirst, later
  // ones cdiAaLater (border hold), per the vendor reference.
  bool _grayRefreshedOnce = false;

  // Async split state (see Uc8179Driver for the contract).
  bool _pendingRefresh = false;
  bool _pendingTurnOff = false;
  bool _pendingPartial = false;
};

PanelDriver& uc8279X4Driver();

}  // namespace freeink
