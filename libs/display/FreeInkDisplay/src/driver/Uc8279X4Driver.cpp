#include "Uc8279X4Driver.h"

#include <Arduino.h>

#include <string.h>

#include <esp_heap_caps.h>

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

// UC8279_aa_prebw_mid — the stock non-flashing previous->current transition
// waveform (Factory.bin DROM 0x3c5d826f, uploaded by FUN_4214d3a0). 5 tables,
// command-prefixed, 42 data bytes each (distinct from the 49-byte display bank).
// Same layout as the UC8179 sibling's kGrayPreBwMid.
constexpr uint8_t PREBW_LUT_LEN = 42;
const uint8_t kXtfPreBwMid[5][PREBW_LUT_LEN + 1] = {
    {0x20, 0x01, 0x06, 0x01, 0x06, 0x06, 0x01, 0x01, 0x01, 0x02, 0x04, 0x00, 0x00, 0x01, 0x01},
    {0x21, 0x01, 0x06, 0x81, 0x06, 0x06, 0x01, 0x01, 0x01, 0x02, 0x04, 0x00, 0x00, 0x01, 0x01},
    {0x22, 0x01, 0x86, 0x81, 0x86, 0x86, 0x01, 0x01, 0x01, 0x82, 0x84, 0x00, 0x00, 0x01, 0x01},
    {0x23, 0x01, 0x46, 0x41, 0x46, 0x46, 0x01, 0x01, 0x01, 0x42, 0x44, 0x00, 0x00, 0x01, 0x01},
    {0x24, 0x01, 0x06, 0x01, 0x06, 0x06, 0x01, 0x01, 0x01, 0x02, 0x44, 0x00, 0x00, 0x01, 0x01},
    // remaining bytes of each 42-byte table are zero (aggregate init).
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
  _grayRefreshedOnce = false;
}

void Uc8279X4Driver::begin(EpdBus& bus) {
  bus.reset(50);
  initController(bus);
  // Framebuffer-sized scratch for the grayscale absolute-plane fold (SPIRAM;
  // falls back to a plain malloc). If it can't be had, copyGrayscale* degrade to
  // the raw-plane fallback path.
  if (_grayBase == nullptr) {
    _grayBase = static_cast<uint8_t*>(heap_caps_malloc(_bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_grayBase == nullptr) _grayBase = static_cast<uint8_t*>(malloc(_bufferSize));
  }
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;
}

void Uc8279X4Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  // CrossPoint's AA path draws its B/W base with an ordinary Fast paint. Right
  // after an AA page, route that base through stock's non-flashing prev->current
  // transition instead of a plain DU — this keeps the gray edge charge in check
  // continuously (like the UC8179 sibling), so the periodic Half scrub works
  // without a full flash. Same guard as UC8179.
  if (mode == RefreshMode::Fast && _redriveAfterGray && _grayRefreshedOnce && _oldPlaneValid && !_needFullClear) {
    transitionGrayscaleBase(bus, fb, turnOff);
    return;
  }
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

// Same geometry/mirroring as streamPlane, but each visible byte is lhs ^ rhs.
void Uc8279X4Driver::streamPlaneXor(EpdBus& bus, uint8_t ramCmd, const uint8_t* lhs, const uint8_t* rhs, bool invert) {
  uint8_t row[128];
  const uint16_t wb = _wb <= sizeof(row) ? _wb : sizeof(row);
  bus.cmd(ramCmd);
  memset(row, 0xFF, wb);
  for (uint16_t y = 0; y < _cfg.gateOffset; y++) bus.data(row, wb);
  static const uint8_t kBitRev[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
                                      0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
  for (uint16_t n = 0; n < _h; n++) {
    const uint16_t y = FREEINK_UC8279X4_ROWREV ? static_cast<uint16_t>(_h - 1 - n) : n;
    const uint8_t* a = lhs + static_cast<uint32_t>(y) * _wb;
    const uint8_t* c = rhs + static_cast<uint32_t>(y) * _wb;
    for (uint16_t i = 0; i < wb; i++) {
      uint8_t b;
      if (FREEINK_UC8279X4_XMIRROR) {
        const uint8_t m = static_cast<uint8_t>(a[wb - 1 - i] ^ c[wb - 1 - i]);
        b = static_cast<uint8_t>((kBitRev[m & 0x0F] << 4) | kBitRev[m >> 4]);
      } else {
        b = static_cast<uint8_t>(a[i] ^ c[i]);
      }
      row[i] = invert ? static_cast<uint8_t>(~b) : b;
    }
    bus.data(row, wb);
  }
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
  // Snapshot the B/W base for a grayscale overlay that may follow (the reader
  // draws this base, then folds it into the absolute AA planes). Harmless for
  // pure B/W paints — it's just a memcpy that the next AA page consumes.
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;
  if (_grayBase != nullptr && fb != nullptr) {
    memcpy(_grayBase, fb, _bufferSize);
    _grayBaseValid = true;
  }
  // Same differential model as the UC8179 sibling: only an EXPLICIT Fast request
  // uses the PTIN/PTOUT DU partial (OLD plane = previous displayed frame). Full
  // AND Half both run the clearing OTP GC waveform — but they seed the OLD plane
  // DIFFERENTLY (this is the load-bearing distinction, copied from UC8179):
  //   * Half = charge SCRUB: OLD = complement of the target, so EVERY pixel
  //     (including white background) is forced through a transition cell and no
  //     WW/BB pixel idles with stale AA charge. A white-seed GC only redraws
  //     black-target pixels and leaves background ghost parked in WW — that was
  //     the residual ghosting seen after fix9's white-seed Half.
  //   * Full = absolute-from-white seed (the known clean full flash).
  // Half is CrossPoint's periodic ghost-cleanup (every getRefreshFrequency()
  // pages) AND the manual force-refresh; it MUST scrub, exactly like UC8179.
  const bool scrub = (mode == RefreshMode::Half);
  const bool fast = (mode == RefreshMode::Fast) && !_needFullClear && _oldPlaneValid;

  streamPlane(bus, CMD_DTM2, fb);
  if (!fast) {
    if (scrub) {
      // Half scrub: OLD = ~target -> every pixel transitions, purging idle charge.
      streamPlane(bus, CMD_DTM1, fb, /*invert=*/true);
    } else {
      // Full flash: seed the OLD plane white across the whole 600-gate scan for
      // the absolute GC-from-white waveform.
      uint8_t whiteRow[128];
      const uint16_t wb = _wb <= sizeof(whiteRow) ? _wb : sizeof(whiteRow);
      memset(whiteRow, 0xFF, wb);
      bus.cmd(CMD_DTM1);
      for (uint16_t y = 0; y < _tresH; y++) bus.data(whiteRow, wb);
    }
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
// Absolute-plane scheme ported from the UC8179 sibling (see the header). The
// delta masks are folded into stock's absolute selectors and sent INVERTED, so
// black=(0,0) and white=(1,1) are distinct buckets (the earlier raw-delta path
// conflated them → white-text ghosting). plane0/LSB -> DTM1 (0x10),
// plane1/MSB -> DTM2 (0x13).
void Uc8279X4Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  if (!lsb) return;
  _absoluteGrayPlanes = false;
  if (_grayBaseValid && _grayBase != nullptr) {
    // plane0 = base | maskLsb  (base bit = 1 for white, 0 for non-white).
    for (uint32_t i = 0; i < _bufferSize; i++) _grayBase[i] = static_cast<uint8_t>(_grayBase[i] | lsb[i]);
    streamPlane(bus, CMD_DTM1, _grayBase, /*invert=*/true);
    _absoluteGrayPlanes = true;
  } else {
    streamPlane(bus, CMD_DTM1, lsb, /*invert=*/true);  // fallback: no base snapshot
  }
  _grayBaseValid = false;  // _grayBase now holds absolute plane0, not the B/W base
}

void Uc8279X4Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (!msb) return;
  if (_absoluteGrayPlanes && _grayBase != nullptr) {
    // plane1 = plane0 ^ maskMsb (streamed inverted). Then recover the B/W base
    // for the post-DRF restore: base = plane0 & plane1 = plane0 & (plane0 ^ msb).
    streamPlaneXor(bus, CMD_DTM2, _grayBase, msb, /*invert=*/true);
    for (uint32_t i = 0; i < _bufferSize; i++)
      _grayBase[i] = static_cast<uint8_t>(_grayBase[i] & (_grayBase[i] ^ msb[i]));
    _grayBaseValid = true;
  } else {
    streamPlane(bus, CMD_DTM2, msb, /*invert=*/true);  // fallback
  }
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
  _grayRefreshedOnce = true;

  powerOnIfNeeded(bus, " 8279x4_gray_PON");

  // The reference rewrites PSR once more between PON and DRF.
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);

  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8279x4_gray");
  // Vendor production behavior: the UC8279 AA path leaves analog power enabled
  // between page refreshes (the UC8179 sibling does the same — its gray_aa sends
  // no POF). Honor only an explicit turnOff.
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4_gray_POF");
    _isScreenOn = false;
  }

  // Restore the clean B/W base to BOTH controller planes (stock gray_aa does the
  // same). `_grayBase` was rebuilt to the base = plane0 & plane1 in
  // copyGrayscaleMsb. This keeps the next Fast B/W diff's OLD plane coherent AND
  // stops a stale gray selector plane from being reused by a later refresh — the
  // core of why absolute planes de-ghost where raw delta planes did not.
  (void)fb;
  if (_grayBaseValid && _grayBase != nullptr) {
    streamPlane(bus, CMD_DTM1, _grayBase);
    streamPlane(bus, CMD_DTM2, _grayBase);
    _oldPlaneValid = true;
    _needFullClear = false;
  }
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;

  // AA still leaves intermediate edge charge a plain DU diff can't neutralize;
  // flag the next B/W page to re-drive every pixel to its target (see
  // displayStart), scrubbing residue with a cheap DU — an explicit Half GC
  // remains the strong purge.
  _redriveAfterGray = true;
}

void Uc8279X4Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  if (!fb) return;
  // Match the UC8179 sibling: an explicit Half (the periodic ghost purge), or a
  // state that can't run the non-flashing transition (first AA page / no valid
  // previous / pending full clear), takes a real B/W activation via display().
  // Otherwise use stock's non-flashing prev->current transition.
  if (fallback == RefreshMode::Half || !_grayRefreshedOnce || !_oldPlaneValid || _needFullClear) {
    display(bus, fb, nullptr, fallback, turnOff);
    return;
  }
  transitionGrayscaleBase(bus, fb, turnOff);
}

void Uc8279X4Driver::transitionGrayscaleBase(EpdBus& bus, const uint8_t* fb, bool turnOff) {
  if (!fb) return;
  // Snapshot the new B/W base for the AA fold that follows.
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;
  if (_grayBase != nullptr) {
    memcpy(_grayBase, fb, _bufferSize);
    _grayBaseValid = true;
  }
  bus.waitBusy(" 8279x4_gray_base_ready");
  // DTM1 retains the previous page's clean B/W base; load the new base into DTM2.
  // The prebw settle waveform drives that transition without an OTP GC flash.
  streamPlane(bus, CMD_DTM2, fb);
  runGrayscalePrecondition(bus);
  // Restore the B/W baseline to DTM1 (an AA upload may overwrite these; the
  // cached snapshot above survives for the fold in copyGrayscale*).
  streamPlane(bus, CMD_DTM1, fb);
  _oldPlaneValid = true;
  _redriveAfterGray = false;
  _needFullClear = false;
  if (turnOff && _isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4_gray_base_POF");
    _isScreenOn = false;
  }
}

// Stock's UC8279_aa_prebw_mid pre-conditioning pass (byte-exact order from
// Factory.bin FUN_4214d3a0): PTIN -> PTL(full window, +120 gate offset) ->
// PSR(REG=1) -> PFS -> gate scan -> CDI 0xD7 -> CCSET -> TSSET(fast) -> upload
// the 5 settle LUTs -> PON -> DRF -> PTOUT. Requires a valid previous page in
// DTM1 and the new base in DTM2.
void Uc8279X4Driver::runGrayscalePrecondition(EpdBus& bus) {
  if (!_oldPlaneValid || !_grayRefreshedOnce) return;
  bus.waitBusy(" 8279x4_gray_pre_ready");

  const uint16_t xEnd = static_cast<uint16_t>(_w - 1);
  const uint16_t yStart = _cfg.gateOffset;
  const uint16_t yEnd = static_cast<uint16_t>(_cfg.gateOffset + _h - 1);
  bus.cmd(CMD_PARTIAL_IN);
  bus.cmd(CMD_PARTIAL_WINDOW);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(static_cast<uint8_t>(xEnd >> 8));
  bus.data(static_cast<uint8_t>(xEnd | 0x07));
  bus.data(static_cast<uint8_t>(yStart >> 8));
  bus.data(static_cast<uint8_t>(yStart & 0xFF));
  bus.data(static_cast<uint8_t>(yEnd >> 8));
  bus.data(static_cast<uint8_t>(yEnd & 0xFF));
  bus.data(0x01);

  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);  // REG=1: run the external settle tables
  bus.data(_cfg.psr1);
  bus.cmd(CMD_PFS);
  bus.data(_cfg.pfs);
  bus.cmd(CMD_GATE_SCAN);
  bus.data(_cfg.gateScan);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiBwFast);  // 0xD7, per FUN_4214d3a0
  bus.cmd(CMD_CCSET);
  bus.data(_cfg.ccset);
  bus.cmd(CMD_TSSET);
  bus.data(_cfg.tssetFast);  // 0x5A
  for (const auto& l : kXtfPreBwMid) {
    bus.cmd(l[0]);
    bus.data(&l[1], PREBW_LUT_LEN);
  }

  powerOnIfNeeded(bus, " 8279x4_gray_pre_PON");
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8279x4_gray_pre_DRF");
  bus.cmd(CMD_PARTIAL_OUT);
}

void Uc8279X4Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;
  if (!bw) {
    _needFullClear = true;
    _oldPlaneValid = false;
    return;
  }
  // Re-seed the OLD plane (0x10) with the clean B/W frame the reader restored —
  // same rationale as the UC8179 sibling. (displayGray already restored the base
  // to both planes; this covers callers that reach cleanup by another route.)
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
