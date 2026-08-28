#pragma once

// UC8279 panel driver — Xteink X4 Pro production runs that ship an UltraChip
// UC8279 (800x480) in place of the SSD1677. NOT the X3's UC8279d (792x528,
// Uc8279Driver) — this variant has its own init (PSR 0x37/0x4D, stock-exact;
// PSR must be rewritten AFTER PON to latch), PLL 0x0E, PFS, a 1-byte CDI,
// a 120-gate offset on the 600-gate scan, and an external-LUT AA grayscale
// path with bitwise-INVERTED planes.
//
// Register sequences, waveform tables, and power ordering come from the Xteink
// X4 Pro 480x800 display hardware reference (vendor R&D doc). Identification:
// VER (0x70) byte2 LUT_VER = 0x02 or 0x68 (0x69 reserved — routed here too, but
// with no AA waveform of its own it uses the 0x68 table; built-in refreshes are
// identical). The boot probe stores that byte in
// BoardConfig::ACTIVE.displayControllerVariant. VALIDATED IN THE FIELD
// (2026-08-19, LUT_VER=0x02 unit): detection, GC full, DU partial (PTL window
// required — see displayStart), and orientation all confirmed on hardware.
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
  // CDI (0x50) — SINGLE byte on this controller. Stock sends the SAME value on
  // every AA refresh (Factory.bin RE: both vtable CDI getters hard-return 0x97;
  // there is NO first/later split — an earlier split to 0xD7 on later refreshes
  // grayed the background, same class of bug as the UC8179 CDI regression).
  uint8_t cdiAa;
  // CDI for the built-in B/W paths — stock writes it on EVERY refresh (RE of
  // the factory FW trigger fns): full/GC and windowed-partial values.
  uint8_t cdiBwFull;
  uint8_t cdiBwFast;
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
  // External-LUT path (ported from the UC8179 sibling). CrossPoint supplies DELTA
  // masks (maskLsb, maskMsb): black/white=(0,0), dark=(1,1), light=(0,1), with a
  // B/W base that is 1 for white and 0 for every non-white pixel. Those are folded
  // into stock's ABSOLUTE selectors so white and black are DISTINCT buckets:
  //   plane0 = base | maskLsb,  plane1 = plane0 ^ maskMsb
  //   -> black=(0,0), dark=(1,0), light=(0,1), white=(1,1)
  // sent INVERTED (as stock does; the 5x49 LUTs were extracted for this encoding).
  // Feeding raw delta planes conflated black & white into one bucket and left the
  // B/W diff baseline unaware of AA edge charge -> white ghosting; the absolute
  // fold + post-DRF base restore (base = plane0 & plane1) fixes both. Single-byte
  // CDI (constant 0x97), PSR rewritten before DRF, panel LEFT POWERED (vendor).
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;
  // Base frame for a grayscale overlay. The periodic clean the reader asks for
  // via a Half fallback must be a TRUE Full GC on this path — the Half is a B/W
  // invert-seed scrub that cannot clear the panel's gray edge charge and leaves
  // ghosting on an AA page. Promote Half->Full here; Fast stays Fast (the
  // absolute AA path self-cleans per page). The pure-B/W menu keeps its Half
  // scrub because it arrives through display()/displayStart, not this entry.
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;

 private:
  void initController(EpdBus& bus);
  // Stream a framebuffer into a RAM plane: 0xFF padding for gates before the
  // visible offset, visible rows in the stock convention (forward order, bytes
  // as-is; FREEINK_UC8279X4_ROWREV/XMIRROR can flip either axis for future
  // sub-variants), then 0xFF padding to the addressed gate count. `invert`
  // bitwise-inverts the image rows (AA planes only, per the vendor reference).
  void streamPlane(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb, bool invert = false);
  // Stream (lhs XOR rhs) into a RAM plane with the same geometry/mirroring as
  // streamPlane (used to build absolute grayscale plane1 = plane0 ^ maskMsb).
  void streamPlaneXor(EpdBus& bus, uint8_t ramCmd, const uint8_t* lhs, const uint8_t* rhs, bool invert = false);
  void powerOnIfNeeded(EpdBus& bus, const char* tag);
  // Stock's non-flashing previous->current AA base transition (FUN_4214d4ac /
  // FUN_4214d3a0). DTM1 holds the previous page's B/W base, DTM2 the new one;
  // the UC8279_aa_prebw_mid settle waveform drives that transition without an OTP
  // GC flash. Ported from the UC8179 sibling; the base is restored to DTM1 after.
  void transitionGrayscaleBase(EpdBus& bus, const uint8_t* fb, bool turnOff);
  void runGrayscalePrecondition(EpdBus& bus);

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

  // Grayscale absolute-plane state (ported from UC8179). `_grayBase` holds the
  // B/W base captured at displayStart; copyGrayscaleLsb folds it into stock's
  // absolute plane0, copyGrayscaleMsb derives plane1 and recovers the base for
  // the post-DRF restore. SPIRAM-backed, framebuffer-sized, allocated in begin().
  uint8_t* _grayBase = nullptr;
  bool _grayBaseValid = false;
  bool _absoluteGrayPlanes = false;
  // True once a grayscale (AA) refresh has run. Gates the non-flashing base
  // transition + precondition (both need a valid previous page in DTM1).
  bool _grayRefreshedOnce = false;
  // Set after every grayscale (AA) refresh. The AA overlay leaves gray edge
  // charge the plain B/W fast diff can't scrub (the B/W baseline records those
  // pixels as white), so it accumulates under rapid page turns → garble. Consumed
  // by the next B/W displayStart to RE-DRIVE every pixel to its target (DTM1 =
  // ~newframe), scrubbing the residue with a cheap DU (no GC flash) — the same
  // trick as _darkBackground, applied once after AA.
  bool _redriveAfterGray = false;

  // Async split state (see Uc8179Driver for the contract).
  bool _pendingRefresh = false;
  bool _pendingTurnOff = false;
  bool _pendingPartial = false;
};

PanelDriver& uc8279X4Driver();

}  // namespace freeink
