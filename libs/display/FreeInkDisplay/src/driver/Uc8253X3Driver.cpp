#include "Uc8253X3Driver.h"

#include <BoardConfig.h>

#include "../lut/Uc8253X3Luts.h"

namespace freeink {
namespace {
// UC8253 command set.
constexpr uint8_t CMD_PANEL_SETTING = 0x00;
constexpr uint8_t CMD_POWER_SETTING = 0x01;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_POWER_OFF_SEQ = 0x03;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint8_t CMD_DTM1 = 0x10;
constexpr uint8_t CMD_DATA_STOP = 0x11;
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;
constexpr uint8_t CMD_DTM2 = 0x13;
constexpr uint8_t CMD_LUT_VCOM = 0x20;
constexpr uint8_t CMD_LUT_WW = 0x21;
constexpr uint8_t CMD_LUT_BW = 0x22;
constexpr uint8_t CMD_LUT_WB = 0x23;
constexpr uint8_t CMD_LUT_BB = 0x24;
constexpr uint8_t CMD_PLL_CONTROL = 0x30;
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;
constexpr uint8_t CMD_RESOLUTION = 0x61;
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;
constexpr uint8_t CMD_VCOM_DC = 0x82;
constexpr uint8_t CMD_LV_SELECTION = 0xE1;
constexpr uint8_t CMD_PARTIAL_WINDOW = 0x90;
constexpr uint8_t CMD_PARTIAL_IN = 0x91;
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;
}  // namespace

const Uc8253X3Config& uc8253X3DefaultConfig() {
  static const Uc8253X3Config cfg = {
      {lut_x3_vcom_normal, lut_x3_ww_normal, lut_x3_bw_normal, lut_x3_wb_normal, lut_x3_bb_normal},
      {lut_x3_vcom_half, lut_x3_ww_half, lut_x3_bw_half, lut_x3_wb_half, lut_x3_bb_half},
      {lut_x3_vcom_fast, lut_x3_ww_fast, lut_x3_bw_fast, lut_x3_wb_fast, lut_x3_bb_fast},
      {lut_x3_vcom_full, lut_x3_ww_full, lut_x3_bw_full, lut_x3_wb_full, lut_x3_bb_full},
      {lut_x3_vcom_gc, lut_x3_ww_gc, lut_x3_bw_gc, lut_x3_wb_gc, lut_x3_bb_gc},
      {lut_x3_vcom_aa_pre_bw_mid, lut_x3_ww_aa_pre_bw_mid, lut_x3_bw_aa_pre_bw_mid, lut_x3_wb_aa_pre_bw_mid,
       lut_x3_bb_aa_pre_bw_mid},
      {lut_x3_vcom_factory_p1, lut_x3_ww_factory_p1, lut_x3_bw_factory_p1, lut_x3_wb_factory_p1,
       lut_x3_bb_factory_p1},
      {lut_x3_vcom_factory_p2, lut_x3_ww_factory_p2, lut_x3_bw_factory_p2, lut_x3_wb_factory_p2,
       lut_x3_bb_factory_p2},
      42,  // controller accepts 42 bytes of each 43-byte array
  };
  return cfg;
}

// Resolution comes from the active BoardProfile (XTEINK_X3), exactly like every
// other driver — the X3 profile is selected at runtime by setDisplayX3() before
// begin() constructs this singleton, so ACTIVE already holds 792x528 here.
Uc8253X3Driver::Uc8253X3Driver(const Uc8253X3Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8253X3Driver::spiHz() const {
  // X3 (UC8253) runs the SPI bus at 16 MHz.
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 16000000;
}

PanelGeometry Uc8253X3Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

void Uc8253X3Driver::loadBank(EpdBus& bus, const Uc8253LutBank& bank) {
  bus.cmdData(CMD_LUT_VCOM, bank.vcom, _cfg.lutLen);
  bus.cmdData(CMD_LUT_WW, bank.ww, _cfg.lutLen);
  bus.cmdData(CMD_LUT_BW, bank.bw, _cfg.lutLen);
  bus.cmdData(CMD_LUT_WB, bank.wb, _cfg.lutLen);
  bus.cmdData(CMD_LUT_BB, bank.bb, _cfg.lutLen);
}

void Uc8253X3Driver::loadBankCdi(EpdBus& bus, uint8_t cdi0, uint8_t cdi1, const Uc8253LutBank& bank) {
  bus.cmdData2(CMD_VCOM_DATA_INTERVAL, cdi0, cdi1);
  loadBank(bus, bank);
}

void Uc8253X3Driver::triggerRefresh(EpdBus& bus, bool turnOff) {
  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" X3_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" X3_DRF");
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" X3_POF");
    _isScreenOn = false;
  }
}

void Uc8253X3Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(0x3F);
  bus.data(0x0A);
  bus.cmd(CMD_RESOLUTION);
  bus.data(0x03);
  bus.data(0x18);
  bus.data(0x02);
  bus.data(0x58);
  bus.cmd(CMD_GATE_SOURCE_START);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);
  bus.cmd(CMD_POWER_OFF_SEQ);
  bus.data(0x20);
  bus.cmd(CMD_POWER_SETTING);
  bus.data(0x07);
  bus.data(0x17);
  bus.data(0x3F);
  bus.data(0x3F);
  bus.data(0x17);
  bus.cmd(CMD_VCOM_DC);
  bus.data(0x24);
  bus.cmd(CMD_BOOSTER_SOFT_START);
  bus.data(0x25);
  bus.data(0x25);
  bus.data(0x3C);
  bus.data(0x37);
  bus.cmd(CMD_PLL_CONTROL);
  bus.data(0x09);
  bus.cmd(CMD_LV_SELECTION);
  bus.data(0x02);

  // UC8253 has no auto-write RAM clear (unlike SSD1677); fill both planes white
  // so the first differential refresh diffs against white, not stale content.
  bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.fillPlane(CMD_DTM2, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);

  _isScreenOn = false;
}

void Uc8253X3Driver::begin(EpdBus& bus) {
  bus.reset(50);  // X3 needs an extra settle after reset
  _redRamSynced = false;
  _initialFullSyncsRemaining = 2;
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
  _inGrayscaleMode = false;
  _grayState = {};
  initController(bus);
}

void Uc8253X3Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

bool Uc8253X3Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;
  if (!_isScreenOn && !turnOff) {
    mode = RefreshMode::Half;  // wake transition gets a stronger waveform
  }
  if (_inGrayscaleMode) {
    grayscaleRevert(bus, fb);
  }

  const bool fastMode = (mode == RefreshMode::Fast);
  const bool halfMode = (mode == RefreshMode::Half);
  const bool forcedFullSync = _forceFullSyncNext;
  const bool doFullSync =
      (!fastMode && !halfMode) || !_redRamSynced || _initialFullSyncsRemaining > 0 || forcedFullSync;
  const bool doHalfSync = halfMode && !doFullSync;
  _grayState.lastBaseWasPartial = !doFullSync;

  if (doFullSync) {
    // _full OEM bank from a white DTM1 baseline (no software prev-frame buffer).
    loadBankCdi(bus, 0x29, 0x07, _cfg.full);
    bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  } else if (doHalfSync) {
    // _half scrub: WW==BW, WB==BB -> drive every pixel to target ignoring DTM1.
    loadBankCdi(bus, 0xA9, 0x07, _cfg.half);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  } else {
    // _fast turbo differential; DTM1 retains the previous frame.
    loadBankCdi(bus, 0x29, 0x07, _cfg.fast);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  }

  // doFullSync re-powers the charge pump even if already on (higher current).
  if (!_isScreenOn || doFullSync) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" X3_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  // Confirm the waveform actually started (BUSY dropped LOW) before handing the
  // CPU back, so displayFinish()'s waitBusy() only rides out the second
  // (LOW->HIGH) phase. Short timeout: a missed edge just falls through to the
  // full two-phase wait in displayFinish().
  {
    const int8_t busyPin = bus.pins().busy;
    const unsigned long t0 = millis();
    while (digitalRead(busyPin) == HIGH && millis() - t0 < 50) delay(1);
  }
  _pendingTurnOff = turnOff;
  _pendingDoFullSync = doFullSync;
  _pendingFastMode = fastMode;
  _pendingRefresh = true;
  return true;
}

void Uc8253X3Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  if (!_pendingRefresh) return;
  _pendingRefresh = false;
  const bool turnOff = _pendingTurnOff;
  const bool doFullSync = _pendingDoFullSync;
  const bool fastMode = _pendingFastMode;

  // ISR-backed wait: displayStart() already confirmed BUSY dropped LOW, so the
  // waveform is running and waitRefreshComplete() will wake on the exact
  // completion edge rather than polling at 1 ms granularity.
  bus.waitRefreshComplete(" X3_DRF");
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" X3_POF");
    _isScreenOn = false;
  }

  if (!fastMode) delay(200);

  uint8_t postConditionPasses = 0;
  if (doFullSync) {
    if (_forceFullSyncNext) postConditionPasses = _forcedConditionPassesNext;
    else if (_initialFullSyncsRemaining == 1) postConditionPasses = 1;
  }
  if (postConditionPasses > 0) {
    const uint16_t xEnd = static_cast<uint16_t>(_w - 1);
    const uint16_t yEnd = static_cast<uint16_t>(_h - 1);
    const uint8_t w[9] = {0x00, 0x00, static_cast<uint8_t>(xEnd >> 8), static_cast<uint8_t>(xEnd & 0xFF),
                          0x00, 0x00, static_cast<uint8_t>(yEnd >> 8), static_cast<uint8_t>(yEnd & 0xFF),
                          0x01};
    loadBankCdi(bus, 0xA9, 0x07, _cfg.normal);  // _normal: OEM normal loader CDI 0xA9
    for (uint8_t i = 0; i < postConditionPasses; i++) {
      bus.cmd(CMD_PARTIAL_IN);
      bus.cmdData(CMD_PARTIAL_WINDOW, w, 9);
      bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
      bus.cmd(CMD_PARTIAL_OUT);
      triggerRefresh(bus, false);
    }
  }

  // Sync DTM1 ("old" RAM) with the current frame for the next fast diff.
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  // Both DTM planes now hold a BW frame, not grayscale planes, so the next
  // displayGrayscaleBase() can take the differential happy path. Without this
  // clear, lsbValid stays true after any grayscale page and pins
  // cleanBaseNeeded on forever.
  _grayState.lsbValid = false;
  _redRamSynced = true;

  // First differential after a full garbles on X3; spend a no-op fast settle of
  // the just-displayed frame so the caller's next diff is clean.
  if (doFullSync) {
    loadBankCdi(bus, 0x29, 0x07, _cfg.fast);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
    triggerRefresh(bus, turnOff);
    bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
  }

  if (doFullSync && _initialFullSyncsRemaining > 0) {
    _initialFullSyncsRemaining--;
  }
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
}

void Uc8253X3Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  // OEM V5.6.33 grayscale base update: write the new frame to DTM2 and fire
  // the "AA-pre-BW(mid)" bank as a differential refresh against the old frame
  // still held in DTM1. Changed pixels get the strong 0xAA/0x55 transition
  // drives, unchanged pixels the gentle 0x20/0x10 reinforcement -- leaving
  // the whole region in the calibrated state the gray nudge bank expects.
  // When the controller state cannot support a clean differential (DTM1
  // unsynced after AA, boot full-syncs pending, or an explicit resync
  // request), fall back to the normal display path and follow it with the
  // settle flavor of the same bank (DTM1 == DTM2 after display()'s post-
  // refresh sync, so only the gentle WW/BB cells fire).
  if (_inGrayscaleMode) {
    // grayscaleRevert scrubs the panel to white and leaves BOTH DTM planes
    // all-white with _redRamSynced set, so DTM1 matches the displayed state
    // and the differential below is valid by construction (white-baseline,
    // the same pattern the full sync uses).
    grayscaleRevert(bus, fb);
  }
  // _grayState.lsbValid means grayscale planes were written over DTM1/DTM2
  // since the last display — the controller RAM no longer holds the displayed
  // BW frame even though _redRamSynced may still read true, so the
  // differential would mis-drive; take the clean fallback path instead.
  const bool cleanBaseNeeded =
      !_redRamSynced || _grayState.lsbValid || _forceFullSyncNext || _initialFullSyncsRemaining > 0;
  if (cleanBaseNeeded) {
    display(bus, fb, nullptr, fallback, /*turnOff=*/false);
    loadBankCdi(bus, 0xA9, 0x07, _cfg.preBwMid);
    triggerRefresh(bus, turnOff);
    return;
  }
  bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  loadBankCdi(bus, 0xA9, 0x07, _cfg.preBwMid);
  triggerRefresh(bus, turnOff);
  // Keep the driver invariant that DTM1 mirrors the displayed frame; the gray
  // plane writes that normally follow overwrite both planes anyway.
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  _redRamSynced = true;
}

void Uc8253X3Driver::preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  // OEM V5.6.33 "AA-pre-BW(mid)" pass: gentle settle of the displayed BW
  // frame (DTM1 == DTM2 == frame after display()'s post-refresh DTM1 sync)
  // that leaves particles receptive to the weak grayscale nudge waveform.
  // Without it a strong base refresh sets pixels too firmly for the gray
  // drive to move. Windowed to the gray region via PTL exactly like the OEM
  // loader (PTIN -> window -> CDI/bank -> refresh -> PTOUT). The PTL Y range
  // is in GATE space (logical row y lives at gate H-1-y, see
  // writeGrayscalePlaneStrip); X is byte-aligned outward since PTL horizontal
  // resolution is 8 pixels.
  if (w == 0 || h == 0 || x >= _w || y >= _h) return;
  // The settle is only meaningful (and only safe) when both DTM planes hold
  // the displayed BW frame. Skip when grayscale planes have been written over
  // them (lsbValid), a grayscale refresh left the RAM unsynced, or the gray
  // bank is still loaded — firing the mid bank's strong BW/WB drives against
  // gray-coded state pairs would corrupt the region.
  if (_inGrayscaleMode || !_redRamSynced || _grayState.lsbValid) return;
  const uint16_t xEndLogical = static_cast<uint16_t>(((x + w - 1) < (_w - 1)) ? (x + w - 1) : (_w - 1));
  const uint16_t yEndLogical = static_cast<uint16_t>(((y + h - 1) < (_h - 1)) ? (y + h - 1) : (_h - 1));
  const uint16_t xs = static_cast<uint16_t>(x & ~7u);
  const uint16_t xe = static_cast<uint16_t>(xEndLogical | 7u);
  const uint16_t gateYStart = static_cast<uint16_t>((_h - 1) - yEndLogical);
  const uint16_t gateYEnd = static_cast<uint16_t>((_h - 1) - y);
  const uint8_t win[9] = {static_cast<uint8_t>(xs >> 8),
                          static_cast<uint8_t>(xs & 0xFF),
                          static_cast<uint8_t>(xe >> 8),
                          static_cast<uint8_t>(xe & 0xFF),
                          static_cast<uint8_t>(gateYStart >> 8),
                          static_cast<uint8_t>(gateYStart & 0xFF),
                          static_cast<uint8_t>(gateYEnd >> 8),
                          static_cast<uint8_t>(gateYEnd & 0xFF),
                          0x01};
  bus.cmd(CMD_PARTIAL_IN);
  bus.cmdData(CMD_PARTIAL_WINDOW, win, 9);
  loadBankCdi(bus, 0xA9, 0x07, _cfg.preBwMid);
  triggerRefresh(bus, /*turnOff=*/false);
  bus.cmd(CMD_PARTIAL_OUT);
}

void Uc8253X3Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  if (!lsb) {
    _grayState.lsbValid = false;
    return;
  }
  bus.sendPlaneFlipped(CMD_DTM1, lsb, _h, _wb);  // LSB plane -> "old" RAM
  bus.cmd(CMD_DATA_STOP);
  _grayState.lsbValid = true;
}

void Uc8253X3Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (!msb || !_grayState.lsbValid) return;
  bus.sendPlaneFlipped(CMD_DTM2, msb, _h, _wb);  // MSB plane -> "new" RAM
  bus.cmd(CMD_DATA_STOP);
}

void Uc8253X3Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                              uint16_t numRows) {
  if (!rows || numRows == 0) return;
  // PTL partial-window in GATE space (logical row y lives at gate H-1-y), rows
  // emitted bottom-first so they land at the same gates the full-frame write
  // uses. Fixes the AA/image banding from windowing in logical space.
  const uint8_t ramCmd = (plane == GrayPlane::Lsb) ? CMD_DTM1 : CMD_DTM2;
  const uint16_t xEnd = static_cast<uint16_t>(_w - 1);
  const uint16_t yEndLogical = static_cast<uint16_t>(yStart + numRows - 1);
  const uint16_t gateYStart = static_cast<uint16_t>((_h - 1) - yEndLogical);
  const uint16_t gateYEnd = static_cast<uint16_t>((_h - 1) - yStart);
  const uint8_t win[9] = {0x00,
                          0x00,
                          static_cast<uint8_t>(xEnd >> 8),
                          static_cast<uint8_t>(xEnd & 0xFF),
                          static_cast<uint8_t>(gateYStart >> 8),
                          static_cast<uint8_t>(gateYStart & 0xFF),
                          static_cast<uint8_t>(gateYEnd >> 8),
                          static_cast<uint8_t>(gateYEnd & 0xFF),
                          0x01};
  bus.cmd(CMD_PARTIAL_IN);
  bus.cmdData(CMD_PARTIAL_WINDOW, win, 9);
  bus.cmd(ramCmd);
  auto txn = bus.beginTxn();
  for (int r = static_cast<int>(numRows) - 1; r >= 0; r--) {
    txn.writeBytes(rows + static_cast<uint32_t>(r) * _wb, _wb);
  }
  txn.end();
  bus.cmd(CMD_PARTIAL_OUT);
  if (plane == GrayPlane::Lsb) _grayState.lsbValid = true;
}

void Uc8253X3Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                 bool factoryMode) {
  (void)fb;
  (void)lut;
  if (!_grayState.lsbValid) return;

  // Differential grayscale leaves the gray bank loaded, so the next BW turn must
  // revert first; factory absolute mode self-cleans.
  _inGrayscaleMode = !factoryMode;
  if (factoryMode) {
    // NOT the OEM standalone grayscale: stock's "X3灰阶" banks (factoryP1/P2)
    // require a dedicated grayscale panel init (PSR 3F 4A, PWR 43 00 78 78 17,
    // VCOM 0x26 — different rails from the B/W init) plus their own DTM data
    // framing. Running them on the B/W-mode rails washes the panel gray.
    // Until that mode switch is ported, factory mode stays on the _full bank.
    loadBankCdi(bus, 0x29, 0x07, _cfg.full);
  } else {
    loadBankCdi(bus, 0x29, 0x07, _cfg.gc);  // OEM 4-level nudge bank
  }
  triggerRefresh(bus, turnOff);

  _redRamSynced = false;
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
  _grayState.lsbValid = false;
}

void Uc8253X3Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw) return;
  // Rebase both planes from the restored BW buffer (same data to DTM1 + DTM2).
  bus.sendPlaneFlipped(CMD_DTM2, bw, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.sendPlaneFlipped(CMD_DTM1, bw, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  // Both planes now hold the rebased BW frame; this is the per-page cleanup the
  // tiled AA reader path runs, so leaving lsbValid true here is what pins
  // cleanBaseNeeded on in steady state.
  _grayState.lsbValid = false;
  _redRamSynced = true;
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
  _inGrayscaleMode = false;
}

void Uc8253X3Driver::grayscaleRevert(EpdBus& bus, const uint8_t* fb) {
  (void)fb;
  if (!_inGrayscaleMode) return;
  _inGrayscaleMode = false;
  // Scrub to clean white: both planes white + the _half scrub bank (CDI 0xA9).
  bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.fillPlane(CMD_DTM2, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  loadBankCdi(bus, 0xA9, 0x07, _cfg.half);
  triggerRefresh(bus, false);
  // Both planes are now all-white (BW-coded), not grayscale planes — clear
  // lsbValid to match, or it stays true forever and forces the cleanBaseNeeded
  // path every page.
  _grayState.lsbValid = false;
  _redRamSynced = true;
}

void Uc8253X3Driver::requestResync(uint8_t settlePasses) {
  _forceFullSyncNext = true;
  _forcedConditionPassesNext = settlePasses;
}

void Uc8253X3Driver::skipInitialResync() {
  _initialFullSyncsRemaining = 0;
  _redRamSynced = true;
}

void Uc8253X3Driver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" X3 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// Per-board waveform/LUT injection: a board that drives a different UC8253 panel
// (different LUTs) supplies its own config without editing this driver — define
// `const Uc8253X3Config& yourConfig();` in namespace freeink and build with
// -DFREEINK_UC8253_X3_CONFIG=yourConfig. Resolution is orthogonal: it always
// comes from that board's BoardProfile. A panel that also differs in init or
// rotation should be its own sibling driver instead.
#ifdef FREEINK_UC8253_X3_CONFIG
const Uc8253X3Config& FREEINK_UC8253_X3_CONFIG();
static const Uc8253X3Config& uc8253X3ActiveConfig() { return FREEINK_UC8253_X3_CONFIG(); }
#else
static const Uc8253X3Config& uc8253X3ActiveConfig() { return uc8253X3DefaultConfig(); }
#endif

PanelDriver& uc8253X3Driver() {
  static Uc8253X3Driver instance(uc8253X3ActiveConfig());
  return instance;
}

}  // namespace freeink
