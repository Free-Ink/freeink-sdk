#include "Uc8279Driver.h"

#include <Arduino.h>

#include <BoardConfig.h>

#include "../lut/Uc8279X3Luts.h"

namespace freeink {
namespace {
// UC8279d command set (UC8279d_B 0.1 datasheet + stock-firmware RE).
constexpr uint8_t CMD_PANEL_SETTING = 0x00;       // PSR
constexpr uint8_t CMD_POWER_OFF = 0x02;           // POF
constexpr uint8_t CMD_POWER_ON = 0x04;            // PON
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;          // DSLP (check code 0xA5)
constexpr uint8_t CMD_DTM1 = 0x10;                // OLD plane in KW mode
constexpr uint8_t CMD_DATA_STOP = 0x11;           // DSP
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;     // DRF
constexpr uint8_t CMD_DTM2 = 0x13;                // NEW plane in KW mode
constexpr uint8_t CMD_LUT_VCOM = 0x20;             // first LUT register (0x20-0x24)
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;  // CDI
constexpr uint8_t CMD_PARTIAL_WINDOW = 0x90;      // PTL
constexpr uint8_t CMD_PARTIAL_IN = 0x91;          // PTIN
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;         // PTOUT
constexpr uint8_t CMD_CCSET = 0xE0;               // CCSET (AA pre-conditioning only)
constexpr uint8_t CMD_TSSET = 0xE5;               // TSSET (AA pre-conditioning only)
}  // namespace

Uc8279Driver::Uc8279Driver()
    : _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8279Driver::spiHz() const {
  // UC8279 serial write timing is rated to 20 MHz, same as UC8253.
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 16000000;
}

PanelGeometry Uc8279Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

void Uc8279Driver::sendScript(EpdBus& bus, const uint8_t* script, uint16_t len) {
  uint16_t i = 0;
  while (i < len) {
    const uint8_t cmd = script[i++];
    const uint8_t n = script[i++];
    bus.cmd(cmd);
    for (uint8_t k = 0; k < n; k++) bus.data(script[i++]);
  }
}

void Uc8279Driver::loadBank(EpdBus& bus, const uint8_t (*bank)[43]) {
  // Command-prefixed banks (BW_GC / BW_DU / XTF_PRE_BW_MID): byte 0 is the LUT
  // register (0x20-0x24), the remaining 42 bytes are its data.
  for (int t = 0; t < 5; t++) {
    bus.cmd(bank[t][0]);
    bus.data(&bank[t][1], 42);
  }
}

void Uc8279Driver::loadXtfAa(EpdBus& bus) {
  // Raw (non-prefixed) 49-byte AA tables: send the register 0x20+t, then the
  // whole table (FUN_42013be0).
  for (int t = 0; t < 5; t++) {
    bus.cmd(static_cast<uint8_t>(CMD_LUT_VCOM + t));
    bus.data(kUc8279X3_XtfAa[t], 49);
  }
}

void Uc8279Driver::grayWindowIn(EpdBus& bus) {
  // PTIN + the full-panel PTL (same 792x528 window the init sets): X 0..791,
  // Y 0..527 in gate space, PT_SCAN=1. Keeps plane writes/refresh at 99-byte
  // rows so they align (normal mode would use the 800x600 frame).
  static const uint8_t kFullWindow[9] = {0x00, 0x00, 0x03, 0x17, 0x00, 0x00, 0x02, 0x0F, 0x01};
  bus.cmd(CMD_PARTIAL_IN);
  bus.cmdData(CMD_PARTIAL_WINDOW, kFullWindow, 9);
}

void Uc8279Driver::triggerGrayRefresh(EpdBus& bus, bool turnOff) {
  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8279_gray_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8279_gray_DRF");
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279_gray_POF");
    _isScreenOn = false;
  }
}

// The stock init (FUN_42014ad4): a blank-MTP module needs the full register
// bring-up. PSR 0x3F sets REG=1 (external LUT); the PTL window defines the
// active 792x528 area in place of TRES; PWR/VDCS supply the drive rails without
// which nothing develops. No plane seed here — the first refresh writes both.
void Uc8279Driver::initController(EpdBus& bus) {
  sendScript(bus, kUc8279X3_Init, sizeof(kUc8279X3_Init));
  _isScreenOn = false;
  _firstRefresh = true;
  _oldPlaneValid = false;
  // Force the first two content paints after boot to GC. CrossPoint paints the
  // boot splash and then home both with FAST; without this, home would be a DU
  // differential over the splash (light waveform -> the splash ghosts through).
  _initialFullsRemaining = 2;
}

void Uc8279Driver::begin(EpdBus& bus) {
  bus.reset(50);
  _forceFullSyncNext = false;
  initController(bus);
}

void Uc8279Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

bool Uc8279Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;  // single-buffer: DTM1 holds the previous frame from displayFinish()'s sync
  // GC vs DU is ONLY a waveform-bank choice — BOTH diff the new frame against the
  // REAL previous frame in DTM1 (the live stock full path FUN_42015786 loads
  // BW_GC and never touches DTM1; BW_GC's WW!=KW / WK!=KK, so it clears via the
  // true old->new transition, not a white baseline). Forcing DTM1 white made a
  // black splash pixel that is white in home read old==new==white -> WW -> no
  // drive -> the splash ghosted through. Use GC (strong clear) for Full/Half, the
  // first paint, a forced resync, and while the boot initial-full budget is
  // unspent (so the first content screen after boot is a real clear, since
  // CrossPoint paints home with FAST); DU only for a Fast request with a baseline.
  const bool useGc = (mode != RefreshMode::Fast) || !_oldPlaneValid || _forceFullSyncNext ||
                     _initialFullsRemaining > 0;

  bus.cmd(CMD_PARTIAL_IN);  // enter the full-panel PTL window set in init

  // KW planes: OLD (0x10) + NEW (0x13). Seed OLD white ONLY on the first paint
  // (no previous frame exists yet); every later refresh diffs against the OLD
  // plane synced to the last displayed frame in displayFinish().
  if (!_oldPlaneValid) {
    bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
  }
  bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);

  // Refresh setup (RE order, FUN_42015786 GC / FUN_4201580a DU): CDI, then the
  // waveform bank. NEITHER B/W path writes E0/E5 — those (0x02/0x5A) belong only
  // to the AA pre-conditioning pass (FUN_42015944), not plain GC/DU refreshes.
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_firstRefresh ? kUc8279X3_CdiFirst : kUc8279X3_CdiLater);
  loadBank(bus, useGc ? kUc8279X3_BwGc : kUc8279X3_BwDu);
  _pendingUsedGc = useGc;

  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8279_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  // Confirm the waveform started (BUSY_N dropped LOW) before returning so
  // displayFinish() only rides out the completion edge.
  {
    const int8_t busyPin = bus.pins().busy;
    const unsigned long t0 = millis();
    while (digitalRead(busyPin) == HIGH && millis() - t0 < 50) delay(1);
  }
  _pendingTurnOff = turnOff;
  _pendingRefresh = true;
  return true;
}

void Uc8279Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8279_DRF");
  bus.cmd(CMD_VCOM_DATA_INTERVAL);  // restore the later-refresh CDI (border hold)
  bus.data(kUc8279X3_CdiLater);

  // Sync the OLD plane with the just-displayed frame so the NEXT refresh (GC or
  // DU) diffs against the real on-screen content — the core of clean clears and
  // ghost-free fast page turns. This MUST happen while still inside the PTIN
  // partial window (before PTOUT): the DTM2 write in displayStart is windowed
  // (792x528 addressing), so DTM1 must use the same window or the two planes
  // misalign and the DU diff drives garbage (the new frame never appears — a
  // full-frame GC hides this because it flashes every pixel regardless).
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _oldPlaneValid = true;
  _firstRefresh = false;
  _forceFullSyncNext = false;
  // Spend one unit of the boot initial-full budget per GC refresh, so the first
  // couple of content screens after boot are strong clears (see displayStart).
  if (_pendingUsedGc && _initialFullsRemaining > 0) _initialFullsRemaining--;

  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279_POF");
    _isScreenOn = false;
  }
}

void Uc8279Driver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;
  _forceFullSyncNext = true;  // next refresh is a full GC flash from white
}

void Uc8279Driver::skipInitialResync() {
  _oldPlaneValid = true;      // caller asserts the panel already holds a valid frame
  _initialFullsRemaining = 0;  // ...so don't force the boot clears
}

void Uc8279Driver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// --- 4-level grayscale / anti-aliasing --------------------------------------
// Structure mirrors the UC8253 X3 sibling (same board/glass, same CrossPoint
// plane generation): LSB -> DTM1 (old), MSB -> DTM2 (new), the XTF_AA external
// LUT resolves the 4 levels. The only UC8279 specifics are the LUT loading
// (raw 49-byte tables) and the single-byte CDI.

void Uc8279Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  if (!lsb) {
    _lsbValid = false;
    return;
  }
  grayWindowIn(bus);
  bus.sendPlaneFlipped(CMD_DTM1, lsb, _h, _wb);  // LSB plane -> "old" RAM
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _lsbValid = true;
}

void Uc8279Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (!msb || !_lsbValid) return;
  grayWindowIn(bus);
  bus.sendPlaneFlipped(CMD_DTM2, msb, _h, _wb);  // MSB plane -> "new" RAM
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
}

void Uc8279Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                            uint16_t numRows) {
  if (!rows || numRows == 0) return;
  // PTL partial-window in GATE space (logical row y lives at gate H-1-y), rows
  // emitted bottom-first so they land on the same gates the full-frame write
  // uses — same idiom as the UC8253 sibling (fixes AA banding).
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
  bus.beginTxn();
  for (int r = static_cast<int>(numRows) - 1; r >= 0; r--) {
    bus.rawWriteBytes(rows + static_cast<uint32_t>(r) * _wb, _wb);
  }
  bus.endTxn();
  bus.cmd(CMD_PARTIAL_OUT);
  if (plane == GrayPlane::Lsb) _lsbValid = true;
}

void Uc8279Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                               bool factoryMode) {
  (void)fb;
  (void)lut;  // waveform is the built-in XTF_AA bank
  if (!_lsbValid) return;
  // Differential grayscale leaves the gray bank/planes loaded, so the next B/W
  // turn must revert first; factory absolute mode self-cleans.
  _inGrayscaleMode = !factoryMode;
  // PSR REG=1 (external LUT) is already set from init and untouched by the B/W
  // path, so just load the AA bank + CDI and refresh (FUN_42015108/42013be0).
  // The refresh MUST run in the partial window (like the plane writes); also
  // resets PTL to full after any per-strip writeGrayscalePlaneStrip windows.
  grayWindowIn(bus);
  loadXtfAa(bus);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_firstRefresh ? kUc8279X3_CdiFirst : kUc8279X3_CdiLater);
  triggerGrayRefresh(bus, turnOff);
  bus.cmd(CMD_PARTIAL_OUT);

  _firstRefresh = false;
  _oldPlaneValid = false;  // gray planes overwrote DTM1/DTM2 — next B/W needs a rebase/clear
  _forceFullSyncNext = false;
  _lsbValid = false;
}

void Uc8279Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  // OEM "AA-pre-BW(mid)" base: settle the frame with XTF_PRE_BW_MID before the
  // gray planes so particles are receptive to the weak AA nudge. When the
  // controller state can't support a clean differential (post-AA, boot fulls
  // pending, or a resync request), take the clean B/W fallback then settle.
  if (_inGrayscaleMode) grayscaleRevert(bus, fb);
  const bool cleanBaseNeeded = !_oldPlaneValid || _lsbValid || _forceFullSyncNext || _initialFullsRemaining > 0;
  if (cleanBaseNeeded) {
    display(bus, fb, nullptr, fallback, /*turnOff=*/false);
    grayWindowIn(bus);
    bus.cmd(CMD_VCOM_DATA_INTERVAL);
    bus.data(kUc8279X3_CdiLater);
    bus.cmd(CMD_CCSET);
    bus.data(kUc8279X3_AaPreE0);
    bus.cmd(CMD_TSSET);
    bus.data(kUc8279X3_AaPreE5);
    loadBank(bus, kUc8279X3_XtfPreBwMid);
    triggerGrayRefresh(bus, turnOff);
    bus.cmd(CMD_PARTIAL_OUT);
    return;
  }
  grayWindowIn(bus);
  bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(kUc8279X3_CdiLater);
  bus.cmd(CMD_CCSET);
  bus.data(kUc8279X3_AaPreE0);
  bus.cmd(CMD_TSSET);
  bus.data(kUc8279X3_AaPreE5);
  loadBank(bus, kUc8279X3_XtfPreBwMid);
  triggerGrayRefresh(bus, turnOff);
  // Keep DTM1 mirroring the displayed frame; the gray plane writes that follow
  // overwrite both planes anyway.
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _oldPlaneValid = true;
}

void Uc8279Driver::preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  if (w == 0 || h == 0 || x >= _w || y >= _h) return;
  // Only safe when both planes hold the displayed B/W frame (not gray, synced).
  if (_inGrayscaleMode || !_oldPlaneValid || _lsbValid) return;
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
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_firstRefresh ? kUc8279X3_CdiFirst : kUc8279X3_CdiLater);
  bus.cmd(CMD_CCSET);
  bus.data(kUc8279X3_AaPreE0);
  bus.cmd(CMD_TSSET);
  bus.data(kUc8279X3_AaPreE5);
  loadBank(bus, kUc8279X3_XtfPreBwMid);
  triggerGrayRefresh(bus, /*turnOff=*/false);
  bus.cmd(CMD_PARTIAL_OUT);
}

void Uc8279Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw) return;
  // Rebase both planes from the restored BW buffer so the next B/W turn has a
  // valid differential baseline (the per-page cleanup the tiled AA reader runs).
  grayWindowIn(bus);
  bus.sendPlaneFlipped(CMD_DTM2, bw, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.sendPlaneFlipped(CMD_DTM1, bw, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_PARTIAL_OUT);
  _lsbValid = false;
  _oldPlaneValid = true;
  _forceFullSyncNext = false;
  _inGrayscaleMode = false;
}

void Uc8279Driver::grayscaleRevert(EpdBus& bus, const uint8_t* fb) {
  (void)fb;
  if (!_inGrayscaleMode) return;
  _inGrayscaleMode = false;
  // Scrub to clean white: both planes white + the strong BW_GC bank.
  grayWindowIn(bus);
  bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.fillPlane(CMD_DTM2, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(kUc8279X3_CdiLater);
  loadBank(bus, kUc8279X3_BwGc);
  triggerGrayRefresh(bus, /*turnOff=*/false);
  bus.cmd(CMD_PARTIAL_OUT);
  _oldPlaneValid = true;  // white baseline
  _lsbValid = false;
}

PanelDriver& uc8279Driver() {
  static Uc8279Driver instance;
  return instance;
}

}  // namespace freeink
