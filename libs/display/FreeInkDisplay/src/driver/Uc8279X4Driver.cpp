#include "Uc8279X4Driver.h"

#include <Arduino.h>

#include <string.h>

#include <BoardConfig.h>

// Orientation of the visible-row stream, switchable per build for field A/B.
// Defaults (0/0) = the stock convention, HARDWARE-CONFIRMED upright on a field
// UC8279 unit: rows forward, no byte/bit mirroring — orientation comes from the
// PSR SHL bit (0x37), which must be written AFTER PON to latch (PON reloads the
// MTP defaults).
#ifndef FREEINK_UC8279X4_ROWREV
#define FREEINK_UC8279X4_ROWREV 0
#endif
#ifndef FREEINK_UC8279X4_XMIRROR
#define FREEINK_UC8279X4_XMIRROR 0
#endif

namespace freeink {
namespace {
// UC8279 (800x480 X4 Pro variant) command set — UC81xx KW family, per the vendor
// X4 Pro display hardware reference.
constexpr uint8_t CMD_PANEL_SETTING = 0x00;       // PSR
constexpr uint8_t CMD_POWER_OFF = 0x02;           // POF
constexpr uint8_t CMD_PFS = 0x03;                 // PFS (power-off sequence)
constexpr uint8_t CMD_POWER_ON = 0x04;            // PON
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;          // DSLP (check code 0xA5)
constexpr uint8_t CMD_DTM1 = 0x10;                // OLD plane in KW mode / AA plane0
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;     // DRF
constexpr uint8_t CMD_DTM2 = 0x13;                // NEW plane in KW mode / AA plane1
constexpr uint8_t CMD_PLL = 0x30;                 // PLL frame rate
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;  // CDI — 1 data byte on this part
constexpr uint8_t CMD_RESOLUTION = 0x61;          // TRES
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;   // GSST (4 data bytes)
constexpr uint8_t CMD_PARTIAL_WINDOW = 0x90;      // PTL (window; stock re-issues per partial)
constexpr uint8_t CMD_PARTIAL_IN = 0x91;          // PTIN
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;         // PTOUT
constexpr uint8_t CMD_CCSET = 0xE0;               // CCSET (cascade/output enable)
constexpr uint8_t CMD_GATE_SCAN = 0xE1;           // gate-scan selection
constexpr uint8_t CMD_TSSET = 0xE5;               // TSSET (forced temperature)

// External AA grayscale waveforms (`xtfAa`), 5 x 49 data bytes, command sent
// separately. Two byte sets exist, selected by the probed LUT_VER (VER byte2):
// 0x02 and 0x68 differ only in the third/fourth frame-group bytes. With the AA
// CDI (0x97, DDX=1) the old/new transition tables map WW->0x21, BW->0x22,
// WB->0x23, BB->0x24; BW/WB carry the dark-gray channel. Only the first 14
// bytes are non-zero; aggregate init zero-fills the rest.
constexpr uint8_t GRAY_LUT_LEN = 49;
struct GrayLut {
  uint8_t cmd;
  uint8_t data[GRAY_LUT_LEN];
};
const GrayLut kXtfAa02[5] = {
    {0x20, {0x01, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // VCOM
    {0x21, {0x01, 0x02, 0x02, 0x41, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // WW
    {0x22, {0x01, 0x02, 0x82, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // BW (dark gray)
    {0x23, {0x01, 0x02, 0x82, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // WB (dark gray)
    {0x24, {0x01, 0x02, 0x02, 0x81, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // BB
};
const GrayLut kXtfAa68[5] = {
    {0x20, {0x01, 0x02, 0x03, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // VCOM
    {0x21, {0x01, 0x02, 0x03, 0x41, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // WW
    {0x22, {0x01, 0x02, 0x83, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // BW (dark gray)
    {0x23, {0x01, 0x02, 0x83, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // WB (dark gray)
    {0x24, {0x01, 0x02, 0x03, 0x81, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // BB
};

const GrayLut* selectAaLuts() {
  // LUT_VER stored by the boot probe. 0x02 has its own table; 0x68 is the newer
  // set. Reserved 0x69 (and anything unknown) falls back to the 0x68 bytes —
  // the reference defines no AA waveform for it, and its init/built-in paths
  // are identical.
  return BoardConfig::ACTIVE.displayControllerVariant == 0x02 ? kXtfAa02 : kXtfAa68;
}
}  // namespace

const Uc8279X4Config& uc8279X4DefaultConfig() {
  static const Uc8279X4Config cfg = {
      0x37,  // psr0: stock value, SHL set (stock streams rows forward with no RAM
             // mirroring and writes PSR between PON and DRF — PON reloads MTP
             // settings, so only post-PON PSR writes latch). REG=1 (external LUT)
             // at init and for AA; built-in refreshes re-assert psr0 & 0xDF = 0x17
      0x4D,  // psr1
      0x20,  // pfs (0x03)
      0x0E,  // pll (0x30) — programmed at init on this variant
      0x02,  // gateScan (0xE1)
      0x02,  // ccset (0xE0)
      0x1E,  // tsset (0xE5) full refresh
      0x5A,  // tssetFast (0xE5) fast/partial refresh
      0x97,  // cdiAa (0x50, 1 byte): constant on EVERY AA refresh, per stock
      0x97,  // cdiBwFull (0x50): stock writes it before every GC/full refresh
      0xD7,  // cdiBwFast (0x50): stock value for the windowed partial
      600,   // tresHeight — addressed 800x600 (480 visible)
      120,   // gateOffset — visible gates start at 120 on this variant
  };
  return cfg;
}

// Visible geometry comes from the active BoardProfile (X4 Pro, 800x480).
Uc8279X4Driver::Uc8279X4Driver(const Uc8279X4Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _tresH(cfg.tresHeight),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8279X4Driver::spiHz() const {
  // UC8279 serial write timing is rated to 20 MHz, same as the rest of the family.
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 16000000;
}

PanelGeometry Uc8279X4Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

// Vendor runtime init: PSR, TRES (800x600), GSST (4x0), PFS, PLL, gate scan.
// PWR/VDCS/BTST stay panel-programmed (OTP/MTP) and are not written here.
void Uc8279X4Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);

  bus.cmd(CMD_RESOLUTION);
  bus.data(static_cast<uint8_t>((_w >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_w & 0xFF));
  bus.data(static_cast<uint8_t>((_tresH >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_tresH & 0xFF));

  bus.cmd(CMD_GATE_SOURCE_START);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);

  bus.cmd(CMD_PFS);
  bus.data(_cfg.pfs);

  bus.cmd(CMD_PLL);
  bus.data(_cfg.pll);

  bus.cmd(CMD_GATE_SCAN);
  bus.data(_cfg.gateScan);

  _isScreenOn = false;
}

void Uc8279X4Driver::begin(EpdBus& bus) {
  bus.reset(50);
  initController(bus);
}

void Uc8279X4Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

void Uc8279X4Driver::streamPlane(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb, bool invert) {
  uint8_t row[128];
  const uint16_t wb = _wb <= sizeof(row) ? _wb : sizeof(row);
  bus.cmd(ramCmd);
  // Gates before the visible window (the 120-gate offset): white.
  memset(row, 0xFF, wb);
  for (uint16_t y = 0; y < _cfg.gateOffset; y++) bus.data(row, wb);
  // Visible rows, stock convention (hardware-confirmed upright): forward order,
  // bytes as-is. The ROWREV/XMIRROR switches (row reversal / reversed byte order
  // + reversed bits) exist for future panel sub-variants whose scan differs.
  // AA planes are sent bitwise-inverted per the vendor reference.
  static const uint8_t kBitRev[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
                                      0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
  for (uint16_t n = 0; n < _h; n++) {
    const uint16_t y = FREEINK_UC8279X4_ROWREV ? static_cast<uint16_t>(_h - 1 - n) : n;
    const uint8_t* src = fb + static_cast<uint32_t>(y) * _wb;
    for (uint16_t i = 0; i < wb; i++) {
      uint8_t b;
      if (FREEINK_UC8279X4_XMIRROR) {
        const uint8_t m = src[wb - 1 - i];
        b = static_cast<uint8_t>((kBitRev[m & 0x0F] << 4) | kBitRev[m >> 4]);
      } else {
        b = src[i];
      }
      row[i] = invert ? static_cast<uint8_t>(~b) : b;
    }
    bus.data(row, wb);
  }
  // Gates after the visible window: white, up to the addressed gate count.
  memset(row, 0xFF, wb);
  for (uint16_t y = _cfg.gateOffset + _h; y < _tresH; y++) bus.data(row, wb);
}

void Uc8279X4Driver::powerOnIfNeeded(EpdBus& bus, const char* tag) {
  if (_isScreenOn) return;  // vendor _powerOn(): no second PON while powered
  bus.cmd(CMD_POWER_ON);
  bus.waitBusy(tag);
  _isScreenOn = true;
}

bool Uc8279X4Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;
  // Same differential model as the UC8179 sibling: full OTP flash on an explicit
  // Full request or the forced first clear, otherwise a PTIN/PTOUT partial whose
  // OLD plane (0x10) holds the previous displayed frame (synced in displayFinish).
  const bool fast = (mode != RefreshMode::Full) && !_needFullClear && _oldPlaneValid;

  streamPlane(bus, CMD_DTM2, fb);
  if (!fast) {
    // Full flash: seed the OLD plane white across the whole 600-gate scan for the
    // absolute GC-from-white waveform.
    uint8_t whiteRow[128];
    const uint16_t wb = _wb <= sizeof(whiteRow) ? _wb : sizeof(whiteRow);
    memset(whiteRow, 0xFF, wb);
    bus.cmd(CMD_DTM1);
    for (uint16_t y = 0; y < _tresH; y++) bus.data(whiteRow, wb);
  } else if (_darkBackground || _redriveAfterGray) {
    // Inverted content: the KW differential idles unchanged pixels, so the
    // light residue of every white->black transition parks in the black
    // background and accumulates between full flashes. Rewrite the OLD plane
    // as the complement of the target: every pixel classifies as changed and
    // is re-driven toward its target — optically invisible on pixels already
    // at their endpoint. displayFinish()'s DTM1 sync restores the baseline.
    streamPlane(bus, CMD_DTM1, fb, /*invert=*/true);
  }
  // Consumed: the white-seed (!fast) or the re-drive above already scrubbed any
  // post-AA gray residue for this frame.
  _redriveAfterGray = false;

  // Built-in refresh setup, byte-for-byte the stock FW trigger order (RE of
  // Factory.bin FUN_4214d050 partial / FUN_4214cfe8 full): CDI first — stock
  // writes the 1-byte CDI on EVERY refresh, 0x97 full / 0xD7 partial — then
  // CCSET/TSSET (+ PFS/gate-scan on partial), PON, the partial window, and a
  // PSR rewrite between PON and DRF (same latch behavior as the AA path).
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(fast ? _cfg.cdiBwFast : _cfg.cdiBwFull);
  bus.cmd(CMD_CCSET);
  bus.data(_cfg.ccset);  // 0x02
  bus.cmd(CMD_TSSET);
  bus.data(fast ? _cfg.tssetFast : _cfg.tsset);  // DU 0x5A / GC 0x1E
  if (fast) {
    bus.cmd(CMD_PFS);
    bus.data(_cfg.pfs);  // 0x03 <- 0x20
    bus.cmd(CMD_GATE_SCAN);
    bus.data(_cfg.gateScan);  // 0xE1 <- 0x02
  }

  powerOnIfNeeded(bus, " 8279x4_PON");

  if (fast) {
    // Stock NEVER issues PTIN without a PTL (0x90) window — with PTL unset the
    // DU scans but develops nothing (first field unit: 443 ms DRF, no image).
    // Full-screen window; gate coords carry the 120-gate visible offset.
    const uint16_t xEnd = _w - 1;
    const uint16_t yStart = _cfg.gateOffset;
    const uint16_t yEnd = _cfg.gateOffset + _h - 1;
    bus.cmd(CMD_PARTIAL_IN);
    bus.cmd(CMD_PARTIAL_WINDOW);
    bus.data(0x00);
    bus.data(0x00);  // x start, byte-aligned (& 0xF8)
    bus.data(static_cast<uint8_t>(xEnd >> 8));
    bus.data(static_cast<uint8_t>(xEnd | 0x07));
    bus.data(static_cast<uint8_t>(yStart >> 8));
    bus.data(static_cast<uint8_t>(yStart & 0xFF));
    bus.data(static_cast<uint8_t>(yEnd >> 8));
    bus.data(static_cast<uint8_t>(yEnd & 0xFF));
    bus.data(0x01);
  }
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(static_cast<uint8_t>(_cfg.psr0 & 0xDF));  // REG cleared -> OTP (0x13)
  bus.data(_cfg.psr1);
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

void Uc8279X4Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8279x4_DRF");
  if (_pendingPartial) bus.cmd(CMD_PARTIAL_OUT);

  // Sync the OLD plane (0x10) with the just-displayed frame so the NEXT partial
  // diffs against it (same ghosting management as the UC8179 sibling).
  streamPlane(bus, CMD_DTM1, fb);
  _oldPlaneValid = true;
  _needFullClear = false;

  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4_POF");
    _isScreenOn = false;
  }
}

void Uc8279X4Driver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;
  _needFullClear = true;
}

void Uc8279X4Driver::skipInitialResync() { _needFullClear = false; }

void Uc8279X4Driver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// --- 4-level grayscale (anti-aliasing) --------------------------------------
// AA plane order is the reverse of the built-in 4-gray order: plane0/LSB -> 0x10,
// plane1/MSB -> 0x13. NOT inverted, unlike stock: stock inverts because ITS
// planes are ABSOLUTE (white=(1,1)); the SDK consumer's planes are DELTA-encoded
// (white AND black = (0,0), only greys flagged — the hardware-proven UC8179
// arrangement). Inverting delta planes sent white through stock's black-drive
// waveform bucket, washing the background gray on every AA refresh
// (field-confirmed on the first UC8279 unit). Non-inverted, white/black land in
// the gentle WW hold bucket, dark grey (1,1) in the strong-drive table, light
// grey (0,1) in the mid-drive table.
void Uc8279X4Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  if (lsb) streamPlane(bus, CMD_DTM1, lsb, /*invert=*/false);
}

void Uc8279X4Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (msb) streamPlane(bus, CMD_DTM2, msb, /*invert=*/false);
}

void Uc8279X4Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                 bool factoryMode) {
  (void)lut;          // waveform is the variant-selected built-in xtfAa table set
  (void)factoryMode;  // 4-level is absolute (defined by the planes)

  // Vendor AA sequence: PSR (REG=1) -> [planes already in RAM via
  // copyGrayscale*] -> 5x49 LUTs -> CDI (constant 0x97) -> PON -> PSR rewrite ->
  // DRF. No CCSET/TSSET writes on this path.
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);  // 0x33: REG=1, external LUT
  bus.data(_cfg.psr1);
  const GrayLut* luts = selectAaLuts();
  for (int i = 0; i < 5; i++) {
    bus.cmd(luts[i].cmd);
    bus.data(luts[i].data, GRAY_LUT_LEN);
  }
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiAa);  // constant 0x97 every AA refresh (stock; no first/later split)

  powerOnIfNeeded(bus, " 8279x4_gray_PON");

  // The reference rewrites PSR once more between PON and DRF.
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);

  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8279x4_gray");
  // Vendor production behavior: the UC8279 AA path intentionally leaves analog
  // power enabled between page refreshes. Honor an explicit turnOff request.
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4_gray_POF");
    _isScreenOn = false;
  }

  // Do NOT reseed the OLD plane (0x10) from `fb` here — on the non-tiled path `fb`
  // holds the MSB gray plane, not the B/W frame. The caller's
  // cleanupGrayscaleBuffers(bw) restores the true B/W baseline. What we DO need:
  // the AA overlay leaves gray edge charge on the panel that a plain B/W fast diff
  // can't scrub (the B/W baseline records those pixels as white), so under rapid
  // page turns it accumulates into garble (slow turns settle/clear; fast don't).
  // Flag the next B/W page to re-drive every pixel to its target (see
  // displayStart), scrubbing the residue each page with a cheap DU — no GC flash.
  (void)fb;
  _redriveAfterGray = true;
}

void Uc8279X4Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw) {
    _needFullClear = true;
    _oldPlaneValid = false;
    return;
  }
  // Re-seed the OLD plane (0x10) with the clean B/W frame the reader restored —
  // same rationale as the UC8179 sibling.
  streamPlane(bus, CMD_DTM1, bw);
  _oldPlaneValid = true;
  _needFullClear = false;
}

// Per-board config injection, same idiom as the other drivers: define
// `const Uc8279X4Config& yourConfig();` in namespace freeink and build with
// -DFREEINK_UC8279_X4_CONFIG=yourConfig.
#ifdef FREEINK_UC8279_X4_CONFIG
const Uc8279X4Config& FREEINK_UC8279_X4_CONFIG();
static const Uc8279X4Config& uc8279X4ActiveConfig() { return FREEINK_UC8279_X4_CONFIG(); }
#else
static const Uc8279X4Config& uc8279X4ActiveConfig() { return uc8279X4DefaultConfig(); }
#endif

PanelDriver& uc8279X4Driver() {
  static Uc8279X4Driver instance(uc8279X4ActiveConfig());
  return instance;
}

}  // namespace freeink
