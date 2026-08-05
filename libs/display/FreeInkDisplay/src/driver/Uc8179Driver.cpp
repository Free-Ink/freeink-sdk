#include "Uc8179Driver.h"

#include <Arduino.h>

#include <string.h>

#include <BoardConfig.h>

namespace freeink {
namespace {
// UC8179 command set (UC8179 datasheet + OEM UC8179_800x480 stream, via Ghidra).
constexpr uint8_t CMD_PANEL_SETTING = 0x00;       // PSR
constexpr uint8_t CMD_POWER_OFF = 0x02;           // POF
constexpr uint8_t CMD_PFS = 0x03;                 // PFS (power-off sequence; PLL is 0x30)
constexpr uint8_t CMD_POWER_ON = 0x04;            // PON
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;  // BTST
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;          // DSLP (check code 0xA5)
constexpr uint8_t CMD_DTM1 = 0x10;                // OLD plane in KW mode
constexpr uint8_t CMD_DTM2 = 0x13;                // NEW plane in KW mode
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;     // DRF
constexpr uint8_t CMD_PARTIAL_IN = 0x91;          // PTIN (partial refresh in)
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;         // PTOUT (partial refresh out)
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;  // CDI
constexpr uint8_t CMD_RESOLUTION = 0x61;          // TRES
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;   // GSST (4 data bytes)
constexpr uint8_t CMD_CCSET = 0xE0;               // CCSET (cascade/output enable)
constexpr uint8_t CMD_GATE_SCAN = 0xE1;           // gate-scan selection
constexpr uint8_t CMD_TSSET = 0xE5;               // TSSET (forced temperature; frame-rate lever)

constexpr uint8_t CDI_INTERVAL = 0x07;  // CDI byte1, constant

// 4-level grayscale (AA) waveform LUTs — stock's REAL grayscale set (the short
// 2-frame LUTs FUN_4214ebd0 actually uploads @app1 DROM 0x3c5d8994..), uploaded
// in custom-LUT mode (PSR REG=1). NOTE: unlike the (dead, grainy) gray_full set,
// here the register command is sent SEPARATELY — blob byte0 is DATA, not the cmd.
// Each LUT is 42 (0x2A) data bytes; only the first ~12 are non-zero. Level select
// by (old=0x10/LSB, new=0x13/MSB): (0,0)=LUTKK black, (0,1)=LUTKW, (1,0)=LUTWK,
// (1,1)=LUTWW white. This is the byte-exact stock set from the known-good 6662faf
// build; the "white-push"/"reset-phase" experiments were chasing a symptom whose
// real cause was the AA-CDI regression (see displayGray) — leave this as stock.
constexpr uint8_t GRAY_LUT_LEN = 42;  // 0x2A data bytes, command sent separately
struct GrayLut {
  uint8_t cmd;
  uint8_t data[GRAY_LUT_LEN];
};
const GrayLut kGrayLuts[5] = {
    {0x20, {0x00, 0x02, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},  // LUTC / VCOM
    {0x21, {0x08, 0x02, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},  // LUTWW (white)
    {0x22, {0x20, 0x02, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},  // LUTKW
    {0x23, {0x20, 0x02, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},  // LUTWK
    {0x24, {0x00, 0x02, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},  // LUTKK (black)
};
}  // namespace

const Uc8179Config& uc8179DefaultConfig() {
  static const Uc8179Config cfg = {
      0x3F,                      // psr0 (init): 0x3B + SHL bit2 set (mirror-X in hardware);
                                 // refresh re-asserts psr0 & 0xDF = 0x1F (OTP + SHL)
      0x0A,                      // psr1
      0x20,                      // pfs (0x03 power-off sequence)
      {0x25, 0x25, 0x3C, 0x25},  // btst (0x06 booster soft-start)
      0x02,                      // gateScan (0xE1)
      0x02,                      // ccset (0xE0)
      0x1E,                      // tsset (0xE5) full refresh (forced-temperature value)
      0x5A,                      // tssetFast (0xE5) fast refresh — REQUIRED: this is the
                                 // frame-rate lever that makes the partial shorter (per RE)
      0x29,                      // cdiActive (0x50, during refresh)
      0xA9,                      // cdiIdle (0x50, restored after)
      600,                       // tresHeight — panel addressed 800x600 (480 visible)
  };
  return cfg;
}

// Visible geometry comes from the active BoardProfile (X4 / X4 Pro, 800x480).
Uc8179Driver::Uc8179Driver(const Uc8179Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _tresH(cfg.tresHeight),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8179Driver::spiHz() const {
  // UC8179 serial write timing is rated to 20 MHz, same as the rest of the family.
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 16000000;
}

PanelGeometry Uc8179Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

// The OEM init (FUN_4214dff8): PSR, TRES (800x600), GSST, PFS, BTST, E1. No plane
// fill, no CDI/VCOM here — those are (re)asserted per refresh. OTP waveforms
// (PSR REG bit cleared at refresh), so no LUT upload.
void Uc8179Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);

  // TRES: HRES (16-bit BE) then VRES (16-bit BE). Width from the visible geometry
  // (800 -> 0x03,0x20), height is the addressed gate count (600 -> 0x02,0x58).
  bus.cmd(CMD_RESOLUTION);
  bus.data(static_cast<uint8_t>((_w >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_w & 0xFF));
  bus.data(static_cast<uint8_t>((_tresH >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_tresH & 0xFF));

  // GSST is a 4-byte register (S_START, banks, G_START x2); the vendor reference
  // writes all four zero bytes.
  bus.cmd(CMD_GATE_SOURCE_START);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);

  bus.cmd(CMD_PFS);
  bus.data(_cfg.pfs);

  bus.cmd(CMD_BOOSTER_SOFT_START);
  bus.data(_cfg.btst[0]);
  bus.data(_cfg.btst[1]);
  bus.data(_cfg.btst[2]);
  bus.data(_cfg.btst[3]);

  bus.cmd(CMD_GATE_SCAN);
  bus.data(_cfg.gateScan);

  _isScreenOn = false;
  _grayRefreshedOnce = false;
}

void Uc8179Driver::begin(EpdBus& bus) {
  bus.reset(50);
  initController(bus);
}

void Uc8179Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

// Stream a framebuffer into RAM plane `ramCmd`, mirrored vertically via row
// reversal (the same sendPlaneFlipped the UC8279 sibling uses — mirror-Y).
// Mirror-X is handled in hardware by the PSR SHL bit, so no per-byte work here.
// White padding then fills the off-screen gates (_h.._tresH); 0xFF = white.
void Uc8179Driver::streamPlane(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb) {
  bus.sendPlaneFlipped(ramCmd, fb, _h, _wb);  // cmd + rows bottom-to-top, one CS burst
  uint8_t whiteRow[128];
  const uint16_t wb = _wb <= sizeof(whiteRow) ? _wb : sizeof(whiteRow);
  memset(whiteRow, 0xFF, wb);
  for (uint16_t y = _h; y < _tresH; y++) bus.data(whiteRow, wb);
}

bool Uc8179Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;
  // Full OTP flash on an explicit Full request or the forced first-clear;
  // otherwise a DIFFERENTIAL partial refresh (PTIN/PTOUT). Fast additionally uses
  // the frame-rate lever (E5=0x5A + 0x03/0xE1) that shortens the waveform.
  //
  // GHOSTING FIX: the OLD plane (0x10) MUST hold the PREVIOUS displayed frame for
  // a partial, not a flat 0xFF. In KW mode the (old,new) pair selects the per-
  // pixel LUT; with old=0xFF only WW/WK fire (white-stays and white->black), so
  // KW (black->white) NEVER runs and last page's text is never erased = heavy
  // ghosting. Feeding the previous frame lets KW clear it. (0x10 is synced to the
  // just-displayed frame in displayFinish; a full refresh reseeds it to white.)
  const bool fast = (mode != RefreshMode::Full) && !_needFullClear && _oldPlaneValid;

  // NEW plane (0x13) = new frame.
  streamPlane(bus, CMD_DTM2, fb);
  if (!fast) {
    // Full flash: seed OLD plane white for the absolute GC-from-white waveform.
    uint8_t whiteRow[128];
    const uint16_t wb = _wb <= sizeof(whiteRow) ? _wb : sizeof(whiteRow);
    memset(whiteRow, 0xFF, wb);
    bus.cmd(CMD_DTM1);
    for (uint16_t y = 0; y < _tresH; y++) bus.data(whiteRow, wb);
  }
  // (Fast: OLD plane already holds the previous frame from the last displayFinish.)

  // --- Refresh setup (exact OEM order) -----------------------------------------
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiActive);  // 0x29
  bus.data(CDI_INTERVAL);
  bus.cmd(CMD_CCSET);
  bus.data(_cfg.ccset);  // 0x02
  bus.cmd(CMD_TSSET);
  bus.data(fast ? _cfg.tssetFast : _cfg.tsset);  // fast 0x5A (frame lever) / full 0x1E
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(static_cast<uint8_t>(_cfg.psr0 & 0xDF));  // REG bit cleared -> OTP
  bus.data(_cfg.psr1);
  if (fast) {
    // Fast-only: PFS/gate-scan re-assert. Full omits these; without them the OTP
    // waveform runs at the full frame count (same duration + garbled).
    bus.cmd(CMD_PFS);
    bus.data(_cfg.pfs);  // 0x03 <- 0x20
    bus.cmd(CMD_GATE_SCAN);
    bus.data(_cfg.gateScan);  // 0xE1 <- 0x02
  }

  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8179_PON");
    _isScreenOn = true;
  }

  if (fast) bus.cmd(CMD_PARTIAL_IN);  // PTIN — whole-panel partial (no 0x90 window)
  bus.cmd(CMD_DISPLAY_REFRESH);
  // Confirm the waveform started (BUSY dropped) before returning, so
  // displayFinish() only rides out the completion edge.
  {
    const int8_t busyPin = bus.pins().busy;
    const unsigned long t0 = millis();
    while (digitalRead(busyPin) == HIGH && millis() - t0 < 50) delay(1);
  }
  _pendingPartial = fast;
  _pendingTurnOff = turnOff;
  _pendingRefresh = true;
  return true;
}

void Uc8179Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8179_DRF");
  if (_pendingPartial) bus.cmd(CMD_PARTIAL_OUT);  // PTOUT closes the partial window
  // Restore the idle CDI (border) after the refresh, as the OEM does.
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiIdle);  // 0xA9
  bus.data(CDI_INTERVAL);

  // Sync the OLD plane (0x10) with the just-displayed frame so the NEXT partial
  // diffs against it (KW clears erased pixels -> no ghosting). This is the piece
  // that makes fast page turns clean.
  streamPlane(bus, CMD_DTM1, fb);
  _oldPlaneValid = true;
  _needFullClear = false;

  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8179_POF");
    _isScreenOn = false;
  }
}

void Uc8179Driver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;
  _needFullClear = true;  // next refresh does a full flash to clear ghosting
}

void Uc8179Driver::skipInitialResync() { _needFullClear = false; }

void Uc8179Driver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8179 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// --- 4-level grayscale (anti-aliasing) --------------------------------------
// Load the two bitplanes (oriented + padded like the B/W path) into controller
// RAM; displayGray() then runs the custom-LUT grayscale waveform. LSB -> 0x10
// ("old"), MSB -> 0x13 ("new"); the (old,new) pair selects the WW/KW/WK/KK LUT
// per pixel for the 4 levels. If the two mid greys come out swapped on hardware,
// swap the LSB/MSB plane assignment here.
void Uc8179Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  if (lsb) streamPlane(bus, CMD_DTM1, lsb);  // 0x10 = LSB / "old" plane
}

void Uc8179Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (msb) streamPlane(bus, CMD_DTM2, msb);  // 0x13 = MSB / "new" plane
}

void Uc8179Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                               bool factoryMode) {
  // fb = the reader's current frame; used to re-seed the B/W baseline below.
  (void)lut;          // waveform comes from the built-in gray LUT set (kGrayLuts)
  (void)factoryMode;  // 4-level is absolute (defined by the planes)
  (void)turnOff;      // gray_aa always powers off at the end (stock cleanup)

  // Custom-LUT 4-level grayscale — the EXACT stock gray_aa stream (FUN_4214ec2c),
  // byte-for-byte: PSR unmasked (0x3F => REG bit5=1 custom LUT, + SHL mirror-X;
  // the B/W path masks to 0x1F/OTP) -> upload the 5 short LUTs (command sent
  // separately, 42 data bytes each) -> CDI 0x29/07 -> PON -> DRF -> POF. Stock
  // sends NO E0/E5/booster here (those belong to the grainy prebw/gray_full
  // paths); adding them scattered the background.
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);  // 0x3F: REG=1 (custom LUT) + KW + SHL mirror-X
  bus.data(_cfg.psr1);
  for (const auto& l : kGrayLuts) {
    bus.cmd(l.cmd);
    bus.data(l.data, GRAY_LUT_LEN);
  }
  // The AA refresh CDI is a CONSTANT 0x29 (app1 vtable[0x118] returns 0x29 every
  // refresh; the known-good 6662faf build used 0x29 unconditionally). The
  // "first 0x29 / later 0xA9" variant from the display-cleanups audit REGRESSED
  // this: 0xA9 (bit7 set) changes the border/VCOM handling on every AA page after
  // the first, which accumulates as ghosting under fast paging and smears when a
  // partial (menu) refresh follows — do NOT reintroduce it.
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiActive);  // 0x29, always
  bus.data(CDI_INTERVAL);
  _grayRefreshedOnce = true;

  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8179_gray_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8179_gray");
  bus.cmd(CMD_POWER_OFF);  // stock gray_aa cleanup
  bus.waitBusy(" 8179_gray_POF");
  _isScreenOn = false;

  // Re-seed the OLD plane (0x10) with this frame so the NEXT B/W page turn runs a
  // fast differential instead of a forced full flash — otherwise the gray LSB
  // plane left in 0x10 makes every AA page turn black-clear. (The reader's
  // non-tiled path never calls cleanupGrayscaleBuffers(), so we seed here; the
  // tiled path refines it later with the exact B/W baseline.) RAM write only —
  // the panel is powered off, which is fine.
  if (fb) {
    streamPlane(bus, CMD_DTM1, fb);
    _oldPlaneValid = true;
    _needFullClear = false;
  } else {
    _needFullClear = true;
    _oldPlaneValid = false;
  }
}

void Uc8179Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw) {
    // No baseline provided — fall back to a full flash on the next B/W refresh.
    _needFullClear = true;
    _oldPlaneValid = false;
    return;
  }
  // Re-seed the OLD plane (0x10) with the clean B/W frame the reader restored, so
  // the NEXT B/W page turn runs a fast differential against a real baseline
  // instead of a forced full flash. The gray refresh left the LSB gray plane in
  // 0x10; without this the next base frame full-flashes (the black clear seen on
  // AA page turns). Mirrors the SSD1677 driver's post-grayscale RED-RAM resync.
  streamPlane(bus, CMD_DTM1, bw);
  _oldPlaneValid = true;
  _needFullClear = false;
}

// Per-board config injection, same idiom as the other drivers: define
// `const Uc8179Config& yourConfig();` in namespace freeink and build with
// -DFREEINK_UC8179_CONFIG=yourConfig.
#ifdef FREEINK_UC8179_CONFIG
const Uc8179Config& FREEINK_UC8179_CONFIG();
static const Uc8179Config& uc8179ActiveConfig() { return FREEINK_UC8179_CONFIG(); }
#else
static const Uc8179Config& uc8179ActiveConfig() { return uc8179DefaultConfig(); }
#endif

PanelDriver& uc8179Driver() {
  static Uc8179Driver instance(uc8179ActiveConfig());
  return instance;
}

}  // namespace freeink
