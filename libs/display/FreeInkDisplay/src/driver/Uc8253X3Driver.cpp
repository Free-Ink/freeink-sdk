#include "Uc8253X3Driver.h"

#include <BoardConfig.h>

#include <cstring>

#include "../lut/Uc8253X3Lut16.h"
#include "../lut/Uc8253X3Luts.h"
#include "../lut/UltraChipDirectGrayLuts.h"

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
  static constexpr Uc8253LutBank directGray = {kUltraChipDirectGray[0], kUltraChipDirectGray[4],
                                               kUltraChipDirectGray[2], kUltraChipDirectGray[3],
                                               kUltraChipDirectGray[1]};
  static constexpr Uc8253X3Config cfg = {
      {lut_x3_vcom_normal, lut_x3_ww_normal, lut_x3_bw_normal, lut_x3_wb_normal, lut_x3_bb_normal},
      {lut_x3_vcom_half, lut_x3_ww_half, lut_x3_bw_half, lut_x3_wb_half, lut_x3_bb_half},
      {lut_x3_vcom_fast, lut_x3_ww_fast, lut_x3_bw_fast, lut_x3_wb_fast, lut_x3_bb_fast},
      {lut_x3_vcom_full, lut_x3_ww_full, lut_x3_bw_full, lut_x3_wb_full, lut_x3_bb_full},
      {lut_x3_vcom_gc, lut_x3_ww_gc, lut_x3_bw_gc, lut_x3_wb_gc, lut_x3_bb_gc},
      {lut_x3_vcom_aa_pre_bw_mid, lut_x3_ww_aa_pre_bw_mid, lut_x3_bw_aa_pre_bw_mid, lut_x3_wb_aa_pre_bw_mid,
       lut_x3_bb_aa_pre_bw_mid},
      42,  // 42 bytes per LUT
      &directGray,
      {
          {x3_lut16::kLut16CrossDissolve_vcom, x3_lut16::kLut16CrossDissolve_ww, x3_lut16::kLut16CrossDissolve_bw,
           x3_lut16::kLut16CrossDissolve_wb, x3_lut16::kLut16CrossDissolve_bb},
          {x3_lut16::kLut16Pass1Coarse_vcom, x3_lut16::kLut16Pass1Coarse_ww, x3_lut16::kLut16Pass1Coarse_bw,
           x3_lut16::kLut16Pass1Coarse_wb, x3_lut16::kLut16Pass1Coarse_bb},
          {x3_lut16::kLut16Pass2Fine_vcom, x3_lut16::kLut16Pass2Fine_ww, x3_lut16::kLut16Pass2Fine_bw,
           x3_lut16::kLut16Pass2Fine_wb, x3_lut16::kLut16Pass2Fine_bb},
          x3_lut16::kPllBase,
          x3_lut16::kPllPass1Coarse,
          x3_lut16::kPllPass2Fine,
          x3_lut16::kPwrPass2Fine[3],
          {x3_lut16::kLut16Step0Erase_vcom, x3_lut16::kLut16Step0Erase_ww, x3_lut16::kLut16Step0Erase_bw,
           x3_lut16::kLut16Step0Erase_wb, x3_lut16::kLut16Step0Erase_bb},
          false,
          x3_lut16::kVcomPass1Coarse,
          x3_lut16::kVcomPass2Fine,
          {x3_lut16::kLut16BipolarScrub_vcom, x3_lut16::kLut16BipolarScrub_ww, x3_lut16::kLut16BipolarScrub_bw,
           x3_lut16::kLut16BipolarScrub_wb, x3_lut16::kLut16BipolarScrub_bb},
      },
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

void Uc8253X3Driver::finishPendingPowerOn(EpdBus& bus) {
  if (!_powerOnPending) return;
  const bool sawLow = _powerOnSawLow;
  _powerOnPending = false;
  _powerOnSawLow = false;
  const int8_t busyPin = bus.pins().busy;
  const uint32_t elapsedMs = static_cast<uint32_t>(millis()) - _powerOnStartMs;
  // Once BUSY_N LOW was observed (or >=20 ms have elapsed), the falling edge has
  // already occurred: skip if already idle HIGH, or wait with ActiveLowReady
  // instead of X3TwoPhase so an already-occurred falling edge never stalls 1000 ms.
  if (busyPin >= 0 && (sawLow || digitalRead(busyPin) == LOW || elapsedMs >= 20u)) {
    if (digitalRead(busyPin) == LOW) {
      bus.waitBusy(BusyPolarity::ActiveLowReady, " X3_PON");
    }
  } else {
    bus.waitBusy(" X3_PON");
  }
  _isScreenOn = true;
  _lastActivityMs = static_cast<uint32_t>(millis());
}

void Uc8253X3Driver::ensurePowerOn(EpdBus& bus) {
  if (_powerOnPending) {
    finishPendingPowerOn(bus);
    return;
  }
  // PON only when the rails are down: a redundant PON on a powered UC8253 gives
  // no BUSY LOW edge, so the X3TwoPhase wait burns its 1000 ms ceiling.
  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" X3_PON");
    _isScreenOn = true;
    _lastActivityMs = static_cast<uint32_t>(millis());
  }
}

void Uc8253X3Driver::triggerRefresh(EpdBus& bus, bool turnOff, const char* label) {
  ensurePowerOn(bus);
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(label);
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" X3_POF");
    _isScreenOn = false;
  } else {
    _lastActivityMs = static_cast<uint32_t>(millis());
  }
}

void Uc8253X3Driver::startAsyncRefresh(EpdBus& bus, PendingDrf kind) {
  ensurePowerOn(bus);
  bus.cmd(CMD_DISPLAY_REFRESH);
  const int8_t busyPin = bus.pins().busy;
  _pendingDrfSawLow = false;
  if (busyPin >= 0) {
    for (int i = 0; i < 20 && digitalRead(busyPin) == HIGH; ++i) {
      delayMicroseconds(10);
    }
    if (digitalRead(busyPin) == HIGH) {
      const unsigned long t0 = millis();
      while (digitalRead(busyPin) == HIGH && millis() - t0 < 50) {
        delay(1);
      }
    }
    _pendingDrfSawLow = (digitalRead(busyPin) == LOW);
  }
  _pendingDrf = kind;
}

void Uc8253X3Driver::drainPendingRefresh(EpdBus& bus) {
  if (_powerOnPending) {
    finishPendingPowerOn(bus);
  }
  const PendingDrf pending = _pendingDrf;
  if (pending == PendingDrf::None) {
    _pendingDrfSawLow = false;
    return;
  }
  const int8_t busyPin = bus.pins().busy;
  const bool drfAlreadyDone = _pendingDrfSawLow && busyPin >= 0 && digitalRead(busyPin) == HIGH;
  _pendingDrf = PendingDrf::None;
  _pendingDrfSawLow = false;
  if (!drfAlreadyDone) {
    bus.waitRefreshComplete(" X3_DRF");
  }
  switch (pending) {
    case PendingDrf::None:
      return;
    case PendingDrf::Gray16Base: {
      const uint8_t pllStd = x3_lut16::kPllStandard;
      bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
      if (_isScreenOn) _lastActivityMs = static_cast<uint32_t>(millis());
      return;
    }
    case PendingDrf::Gray16Pass1: {
      const uint8_t pllStd = x3_lut16::kPllStandard;
      const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
      bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
      bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
      if (_isScreenOn) _lastActivityMs = static_cast<uint32_t>(millis());
      return;
    }
    case PendingDrf::Gray16Pass2:
    case PendingDrf::Gray16Pass2TurnOff: {
      const uint8_t pllStd = x3_lut16::kPllStandard;
      bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
      bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
      if (pending == PendingDrf::Gray16Pass2TurnOff && _isScreenOn) {
        const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
        bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
        bus.cmd(CMD_POWER_OFF);
        bus.waitBusy(" X3_POF");
        _isScreenOn = false;
      } else if (_isScreenOn) {
        // Park VCOM_DC at 0x00 (-0.10V ~= V_source) while HV rails stay on to prevent photoconductive leakage.
        const uint8_t vcomIdle = x3_lut16::kVcomDcIdle;
        bus.cmdData(CMD_VCOM_DC, &vcomIdle, 1);
        _lastActivityMs = static_cast<uint32_t>(millis());
      } else {
        const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
        bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
      }
      return;
    }
  }
}

void Uc8253X3Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  (void)fb;
  drainPendingRefresh(bus);
}

void Uc8253X3Driver::grayWindowIn(EpdBus& bus) {
  const uint16_t xEnd = _w - 1;
  const uint16_t yEnd = _h - 1;
  const uint8_t window[9] = {0, 0, static_cast<uint8_t>(xEnd >> 8), static_cast<uint8_t>(xEnd),
                             0, 0, static_cast<uint8_t>(yEnd >> 8), static_cast<uint8_t>(yEnd),
                             1};
  bus.cmd(CMD_PARTIAL_IN);
  bus.cmdData(CMD_PARTIAL_WINDOW, window, sizeof(window));
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
  const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
  bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
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
  drainPendingRefresh(bus);
  bus.reset(50);  // X3 needs an extra settle after reset
  _powerOnPending = false;
  _powerOnSawLow = false;
  _pendingDrfSawLow = false;
  _hasPresentedSinceInit = false;
  _polarityDirty = false;
  _redRamSynced = false;
  _initialFullSyncsRemaining = 2;
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
  _inGrayscaleMode = false;
  _grayState = {};
  _absoluteInput = false;
  _directGrayPass = false;
  _directGrayOnPanel = false;
  _gray16CleanupActive = false;
  _pendingDrf = PendingDrf::None;
  _lastActivityMs = 0;
  _powerOnStartMs = 0;
  initController(bus);
}

void Uc8253X3Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  drainPendingRefresh(bus);
  (void)prev;
  const bool leavingDirectGray = _directGrayOnPanel;
  _absoluteInput = false;
  _directGrayPass = false;
  if (_directGrayOnPanel) {
    // Controller RAM contains selectors, not the displayed B/W baseline.
    _directGrayOnPanel = false;
    _redRamSynced = false;
    _inGrayscaleMode = false;
  }
  // Only promote to Half on the first refresh after begin()/deepSleep() wake
  // or after a polarity flip; warm CMD_POWER_OFF (0x02) retains DTM1/DTM2 SRAM.
  if ((!_hasPresentedSinceInit && !turnOff) || (_polarityDirty && mode == RefreshMode::Fast)) {
    mode = RefreshMode::Half;
  }
  _polarityDirty = false;
  if (_inGrayscaleMode) {
    grayscaleRevert(bus, fb);
  }
  const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
  bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);

  const bool fastMode = (mode == RefreshMode::Fast);
  const bool halfMode = (mode == RefreshMode::Half);
  const bool forcedFullSync = _forceFullSyncNext;
  const bool doFullSync =
      (!fastMode && !halfMode) || !_redRamSynced || _initialFullSyncsRemaining > 0 || forcedFullSync;
  const bool doHalfSync = halfMode && !doFullSync;
  _grayState.lastBaseWasPartial = !doFullSync;

  if (leavingDirectGray && doFullSync) {
    // Establish the destination before the full recovery's visible flash.
    // HALF pairs WW==BW and WB==BB, making its drive independent of old RAM.
    loadBankCdi(bus, 0xA9, 0x07, _cfg.half);
    grayWindowIn(bus);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
    bus.cmd(CMD_PARTIAL_OUT);
    triggerRefresh(bus, false, " X3_BW_TARGET_DRF");
  }

  const bool useCleanupScrub = doHalfSync && _gray16CleanupActive && hasLutBank(_cfg.lut16.bipolarScrub);
  const bool useCleanupFast = fastMode && !doFullSync && _gray16CleanupActive && hasLutBank(_cfg.lut16.preBwMid);
  const uint8_t pllStd = x3_lut16::kPllStandard;
  const uint8_t pllBase = static_cast<uint8_t>(_cfg.lut16.pllBase & 0x0Fu);

  if (doFullSync) {
    // _full OEM bank from a white DTM1 baseline (no software prev-frame buffer).
    loadBankCdi(bus, 0x29, 0x07, _cfg.full);
    bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  } else if (useCleanupScrub) {
    // Leaving 16-gray cleanup on Half: single-DRF history-independent bipolarScrub (CDI 0xA9, N2OCP=1).
    bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
    bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
    loadBankCdi(bus, 0xA9, 0x07, _cfg.lut16.bipolarScrub);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  } else if (doHalfSync) {
    // _half scrub: WW==BW, WB==BB -> drive every pixel to target ignoring DTM1.
    loadBankCdi(bus, 0xA9, 0x07, _cfg.half);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  } else if (useCleanupFast) {
    // Leaving 16-gray cleanup on Fast: cross-dissolve (CDI 0xA9, N2OCP=1) clears prior ink and drives black strokes
    // cleanly.
    bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
    bus.cmdData(CMD_PLL_CONTROL, &pllBase, 1);
    loadBankCdi(bus, 0xA9, 0x07, _cfg.lut16.preBwMid);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  } else {
    // _fast turbo differential; DTM1 retains the previous frame.
    loadBankCdi(bus, 0x29, 0x07, _cfg.fast);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  }

  ensurePowerOn(bus);
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" X3_DRF");
  if (useCleanupFast) {
    bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
  }
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" X3_POF");
    _isScreenOn = false;
  } else {
    _lastActivityMs = static_cast<uint32_t>(millis());
  }

  if (!fastMode && !useCleanupScrub) delay(200);

  uint8_t postConditionPasses = 0;
  if (doFullSync) {
    if (forcedFullSync)
      postConditionPasses = _forcedConditionPassesNext;
    else if (_initialFullSyncsRemaining == 1)
      postConditionPasses = 1;
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

  // On !doFullSync, CDI 0x29/0xA9 sets N2OCP=1 so full-frame DRF already copied
  // DTM2 -> DTM1 in UC8253 hardware; only re-upload DTM1 on full sync / PTL passes.
  if (doFullSync) {
    bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
  }
  // Both DTM planes now hold a BW frame, not grayscale planes, so the next
  // displayGrayscaleBase() can take the differential happy path. Without this
  // clear, lsbValid stays true after any grayscale page and pins
  // cleanBaseNeeded on forever.
  _grayState.lsbValid = false;
  _redRamSynced = true;
  _gray16CleanupActive = false;
  _hasPresentedSinceInit = true;

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
  if (!turnOff && _isScreenOn) {
    const uint8_t vcomIdle = x3_lut16::kVcomDcIdle;
    bus.cmdData(CMD_VCOM_DC, &vcomIdle, 1);
    _lastActivityMs = static_cast<uint32_t>(millis());
  }
}

void Uc8253X3Driver::displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y,
                                   uint16_t w, uint16_t h, bool turnOff) {
  drainPendingRefresh(bus);
  if (!fb || w == 0 || h == 0 || x >= _w || y >= _h) return;

  // Fall back to whole-panel Fast whenever controller RAM cannot safely serve
  // a PTL windowed differential update.
  if (turnOff || !_hasPresentedSinceInit || _polarityDirty || !_redRamSynced || _initialFullSyncsRemaining > 0 ||
      _forceFullSyncNext || _inGrayscaleMode || _directGrayOnPanel || _grayState.lsbValid) {
    display(bus, fb, prev, RefreshMode::Fast, turnOff);
    return;
  }

  const uint16_t xEndLogical = static_cast<uint16_t>(((x + w - 1) < (_w - 1)) ? (x + w - 1) : (_w - 1));
  const uint16_t yEndLogical = static_cast<uint16_t>(((y + h - 1) < (_h - 1)) ? (y + h - 1) : (_h - 1));
  const uint16_t xs = static_cast<uint16_t>(x & ~7u);
  const uint16_t xe = static_cast<uint16_t>(xEndLogical | 7u);
  if (xs == 0 && xe == static_cast<uint16_t>(_w - 1) && y == 0 && yEndLogical == static_cast<uint16_t>(_h - 1)) {
    display(bus, fb, prev, RefreshMode::Fast, turnOff);
    return;
  }

  const uint16_t gateYStart = static_cast<uint16_t>((_h - 1) - yEndLogical);
  const uint16_t gateYEnd = static_cast<uint16_t>((_h - 1) - y);
  const uint16_t xByteOffset = static_cast<uint16_t>(xs >> 3);
  const uint16_t winWb = static_cast<uint16_t>(((xe - xs) >> 3) + 1u);
  const uint8_t win[9] = {static_cast<uint8_t>(xs >> 8),
                          static_cast<uint8_t>(xs & 0xFF),
                          static_cast<uint8_t>(xe >> 8),
                          static_cast<uint8_t>(xe & 0xFF),
                          static_cast<uint8_t>(gateYStart >> 8),
                          static_cast<uint8_t>(gateYStart & 0xFF),
                          static_cast<uint8_t>(gateYEnd >> 8),
                          static_cast<uint8_t>(gateYEnd & 0xFF),
                          0x01};

  const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
  const uint8_t pllStd = x3_lut16::kPllStandard;
  const uint8_t pllBase = static_cast<uint8_t>(_cfg.lut16.pllBase & 0x0Fu);
  const bool useCleanupFast = _gray16CleanupActive && hasLutBank(_cfg.lut16.preBwMid);

  bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
  bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
  if (useCleanupFast) {
    bus.cmdData(CMD_PLL_CONTROL, &pllBase, 1);
    loadBankCdi(bus, 0xA9, 0x07, _cfg.lut16.preBwMid);
  } else {
    bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
    loadBankCdi(bus, 0xA9, 0x07, _cfg.fast);
  }

  // Match writeGrayscalePlaneStrip: close each PTIN RAM burst with PTOUT
  // (never CMD_DATA_STOP inside PTIN) and run ensurePowerOn outside PTIN.
  const auto writeWindowPlane = [&](uint8_t ramCmd) {
    bus.cmd(CMD_PARTIAL_IN);
    bus.cmdData(CMD_PARTIAL_WINDOW, win, sizeof(win));
    bus.cmd(ramCmd);
    auto txn = bus.beginTxn();
    alignas(4) uint8_t bounce[256];
    size_t buffered = 0;
    for (int row = static_cast<int>(yEndLogical); row >= static_cast<int>(y); --row) {
      const uint8_t* src = fb + static_cast<size_t>(row) * _wb + xByteOffset;
      if (winWb > sizeof(bounce)) {
        if (buffered > 0) {
          txn.writeBytes(bounce, static_cast<uint16_t>(buffered));
          buffered = 0;
        }
        txn.writeBytes(src, winWb);
        continue;
      }
      if (buffered + winWb > sizeof(bounce)) {
        txn.writeBytes(bounce, static_cast<uint16_t>(buffered));
        buffered = 0;
      }
      std::memcpy(bounce + buffered, src, winWb);
      buffered += winWb;
    }
    if (buffered > 0) {
      txn.writeBytes(bounce, static_cast<uint16_t>(buffered));
    }
    txn.end();
    bus.cmd(CMD_PARTIAL_OUT);
  };

  writeWindowPlane(CMD_DTM2);
  ensurePowerOn(bus);
  bus.cmd(CMD_PARTIAL_IN);
  bus.cmdData(CMD_PARTIAL_WINDOW, win, sizeof(win));
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" X3_DRF");
  bus.cmd(CMD_PARTIAL_OUT);
  writeWindowPlane(CMD_DTM1);

  if (useCleanupFast) {
    bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
  }
  _grayState.lastBaseWasPartial = true;
  if (_isScreenOn) {
    const uint8_t vcomIdle = x3_lut16::kVcomDcIdle;
    bus.cmdData(CMD_VCOM_DC, &vcomIdle, 1);
    _lastActivityMs = static_cast<uint32_t>(millis());
  }
}

void Uc8253X3Driver::beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback,
                                    bool turnOff) {
  drainPendingRefresh(bus);
  if (mode == GrayscaleMode::Direct && _cfg.directGray) {
    // Stage complete planes before the only display activation. No B/W image is
    // shown.
    _absoluteInput = true;
    _directGrayPass = true;
    _grayState = {};
    _redRamSynced = false;
    _inGrayscaleMode = false;
    const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
    bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
    bus.cmdData2(CMD_PANEL_SETTING, 0x3F, 0x0A);
    return;
  }
  displayGrayscaleBase(bus, fb, fallback, turnOff);
  _absoluteInput = mode == GrayscaleMode::Absolute;
}

void Uc8253X3Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  (void)displayGrayscaleBaseImpl(bus, fb, fallback, turnOff, /*gray16Pass=*/false, /*allowDefer=*/false);
}

void Uc8253X3Driver::displayGray16Base(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  (void)displayGrayscaleBaseImpl(bus, fb, fallback, turnOff, /*gray16Pass=*/true, /*allowDefer=*/false);
}

bool Uc8253X3Driver::displayGray16BaseStart(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  if (!fb) {
    drainPendingRefresh(bus);
    return false;
  }
  return displayGrayscaleBaseImpl(bus, fb, fallback, turnOff, /*gray16Pass=*/true, /*allowDefer=*/true);
}

bool Uc8253X3Driver::displayGrayscaleBaseImpl(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff,
                                              bool gray16Pass, bool allowDefer) {
  drainPendingRefresh(bus);
  _absoluteInput = false;
  _directGrayPass = false;
  const bool useLut16 = gray16Pass && supportsGray16Planes();
  const uint8_t pllStd = x3_lut16::kPllStandard;
  const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
  const uint8_t pllBase = static_cast<uint8_t>(_cfg.lut16.pllBase & 0x0Fu);
  const bool polarityChanged = _polarityDirty;
  _polarityDirty = false;
  if (polarityChanged && fallback == RefreshMode::Fast) {
    fallback = RefreshMode::Half;
  }

  // Single-DRF DC-balanced bipolar GC scrub for Gray16 HALF_REFRESH:
  // WW==BW and WB==BB drive every pixel solely from DTM2 (history-independent of DTM1),
  // and CDI 0xA9 (N2OCP=1) copies DTM2 -> DTM1 in hardware at DRF completion.
  if (useLut16 && hasLutBank(_cfg.lut16.bipolarScrub) && fallback == RefreshMode::Half && !_forceFullSyncNext &&
      _initialFullSyncsRemaining == 0) {
    _directGrayOnPanel = false;
    _inGrayscaleMode = false;
    _grayState.lsbValid = false;
    _gray16CleanupActive = false;
    bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
    bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
    bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
    loadBankCdi(bus, 0xA9, 0x07, _cfg.lut16.bipolarScrub);
    _redRamSynced = true;
    _hasPresentedSinceInit = true;
    if (allowDefer && !turnOff) {
      startAsyncRefresh(bus, PendingDrf::Gray16Base);
      return true;
    }
    triggerRefresh(bus, /*turnOff=*/false);
    bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
    if (turnOff && _isScreenOn) {
      bus.cmd(CMD_POWER_OFF);
      bus.waitBusy(" X3_POF");
      _isScreenOn = false;
    } else if (_isScreenOn) {
      _lastActivityMs = static_cast<uint32_t>(millis());
    }
    return false;
  }

  const bool revertedToWhite = _inGrayscaleMode;
  if (_inGrayscaleMode) {
    // grayscaleRevert scrubs the panel to white and leaves BOTH DTM planes
    // all-white with _redRamSynced set.
    grayscaleRevert(bus, fb);
  }
  // _grayState.lsbValid means grayscale planes were written over DTM1/DTM2
  // since the last display — the controller RAM no longer holds the displayed
  // BW frame even though _redRamSynced may still read true, so the
  // differential would mis-drive; take the clean fallback path instead.
  const bool cleanBaseNeeded = (fallback == RefreshMode::Full) || polarityChanged ||
                               (useLut16 && fallback == RefreshMode::Half) || !_redRamSynced || _grayState.lsbValid ||
                               _forceFullSyncNext || _initialFullSyncsRemaining > 0;
  if (cleanBaseNeeded) {
    _gray16CleanupActive = false;
    if (useLut16) {
      bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
      bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
      bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
    }
    const RefreshMode cleanFallback =
        (_grayState.lsbValid && fallback == RefreshMode::Fast) ? RefreshMode::Half : fallback;
    display(bus, fb, nullptr, cleanFallback, /*turnOff=*/false);
    bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
    loadBankCdi(bus, 0xA9, 0x07, _cfg.preBwMid);
    triggerRefresh(bus, turnOff);
    _hasPresentedSinceInit = true;
    return false;
  }

  _gray16CleanupActive = false;
  if (useLut16) {
    bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
    bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
    // Optional Step 0 erase (@ 80 Hz 0x09) when step0Enable is set.
    if (!revertedToWhite && _cfg.lut16.step0Enable && hasLutBank(_cfg.lut16.step0Erase)) {
      bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
      loadBankCdi(bus, 0x29, 0x07, _cfg.lut16.step0Erase);
      triggerRefresh(bus, /*turnOff=*/false);
    }
    // Single-pass cross-dissolve (@ pllBase = 0x09 80 Hz; CDI 0xA9 sets N2OCP=1
    // so hardware copies DTM2 -> DTM1 on completion). Restore 0x09 idle PLL afterward.
    bus.cmdData(CMD_PLL_CONTROL, &pllBase, 1);
    bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
    loadBankCdi(bus, 0xA9, 0x07, _cfg.lut16.preBwMid);
    _redRamSynced = true;
    _hasPresentedSinceInit = true;
    if (allowDefer && !turnOff) {
      startAsyncRefresh(bus, PendingDrf::Gray16Base);
      return true;
    }
    triggerRefresh(bus, /*turnOff=*/false);
    bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
    if (turnOff && _isScreenOn) {
      bus.cmd(CMD_POWER_OFF);
      bus.waitBusy(" X3_POF");
      _isScreenOn = false;
    } else if (_isScreenOn) {
      _lastActivityMs = static_cast<uint32_t>(millis());
    }
    return false;
  }

  bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
  bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  loadBankCdi(bus, 0xA9, 0x07, _cfg.preBwMid);
  triggerRefresh(bus, turnOff);
  // Keep the driver invariant that DTM1 mirrors the displayed frame; the gray
  // plane writes that normally follow overwrite both planes anyway.
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  _redRamSynced = true;
  _hasPresentedSinceInit = true;
  if (_isScreenOn) {
    _lastActivityMs = static_cast<uint32_t>(millis());
  }
  return false;
}

void Uc8253X3Driver::preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  drainPendingRefresh(bus);
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
  // the displayed BW frame. Skip when grayscale planes or 3-state Gray16
  // cleanup planes (DTM1 != DTM2) sit in controller RAM, or when the gray
  // bank is still loaded — firing the mid bank's strong BW/WB drives against
  // mismatched planes would corrupt the region.
  if (_inGrayscaleMode || _polarityDirty || !_redRamSynced || _grayState.lsbValid || _gray16CleanupActive) return;
  const uint8_t pllStd = x3_lut16::kPllStandard;
  const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
  bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
  bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
  bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
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
  if (_isScreenOn) {
    const uint8_t vcomIdle = x3_lut16::kVcomDcIdle;
    bus.cmdData(CMD_VCOM_DC, &vcomIdle, 1);
    _lastActivityMs = static_cast<uint32_t>(millis());
  }
}

void Uc8253X3Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  drainPendingRefresh(bus);
  if (_directGrayPass) {
    writeGrayscalePlaneStrip(bus, GrayPlane::Lsb, lsb, 0, _h);
    return;
  }
  if (!lsb) {
    _grayState.lsbValid = false;
    return;
  }
  bus.sendPlaneFlipped(CMD_DTM1, lsb, _h, _wb);  // LSB plane -> "old" RAM
  bus.cmd(CMD_DATA_STOP);
  _grayState.lsbValid = true;
}

void Uc8253X3Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  drainPendingRefresh(bus);
  if (!msb || !_grayState.lsbValid) return;
  if (_directGrayPass) {
    writeGrayscalePlaneStrip(bus, GrayPlane::Msb, msb, 0, _h);
    return;
  }
  bus.sendPlaneFlipped(CMD_DTM2, msb, _h, _wb);  // MSB plane -> "new" RAM
  bus.cmd(CMD_DATA_STOP);
}

void Uc8253X3Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                              uint16_t numRows) {
  drainPendingRefresh(bus);
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

bool Uc8253X3Driver::writeGrayscalePlaneGateStream(EpdBus& bus, GrayPlane plane, void* ctx,
                                                   size_t (*nextChunk)(void* ctx, const uint8_t** outChunk)) {
  drainPendingRefresh(bus);
  if (_directGrayPass || _directGrayOnPanel || !nextChunk) return false;
  const uint8_t ramCmd = (plane == GrayPlane::Lsb) ? CMD_DTM1 : CMD_DTM2;
  const size_t bytesSent = bus.sendStream(ramCmd, nextChunk, ctx);
  bus.cmd(CMD_DATA_STOP);
  if (bytesSent != _bufferSize) {
    _grayState.lsbValid = false;
    _redRamSynced = false;
    _gray16CleanupActive = false;
    return false;
  }
  if (plane == GrayPlane::Lsb) _grayState.lsbValid = true;
  return true;
}

void Uc8253X3Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                 bool factoryMode) {
  drainPendingRefresh(bus);
  (void)fb;
  (void)lut;
  if (!_grayState.lsbValid) return;

  const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
  bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);

  if (_directGrayPass) {
    bus.cmdData2(CMD_VCOM_DATA_INTERVAL, 0xA9, 0x07);
    const auto& bank = *_cfg.directGray;
    loadBank(bus, bank);
    // Strip uploads leave the last band selected; refresh the entire visible
    // panel.
    grayWindowIn(bus);
    triggerRefresh(bus, turnOff, " X3_DIRECT_GRAY_DRF");
    bus.cmd(CMD_PARTIAL_OUT);
    if (!turnOff && _isScreenOn) {
      const uint8_t vcomIdle = x3_lut16::kVcomDcIdle;
      bus.cmdData(CMD_VCOM_DC, &vcomIdle, 1);
      _lastActivityMs = static_cast<uint32_t>(millis());
    }
    _directGrayOnPanel = true;
    _hasPresentedSinceInit = true;
    _absoluteInput = false;
    _directGrayPass = false;
    _grayState = {};
    _redRamSynced = false;
    _inGrayscaleMode = false;
    _initialFullSyncsRemaining = 0;
    _forceFullSyncNext = false;
    _forcedConditionPassesNext = 0;
    return;
  }

  // Differential grayscale leaves the gray bank loaded, so the next BW turn must
  // revert first; factory absolute mode self-cleans.
  _inGrayscaleMode = _absoluteInput || !factoryMode;
  if (_absoluteInput) {
    // (DTM1, DTM2): 00 black, 10 dark, 01 light, 11 white. The B/W base
    // already supplies the endpoints. Retain the two calibrated gray drives
    // and idle both endpoints; no mask conversion or extra framebuffer is needed.
    const Uc8253LutBank absoluteGc = {_cfg.gc.vcom, _cfg.gc.bb, _cfg.gc.bw, _cfg.gc.ww, _cfg.gc.bb};
    loadBankCdi(bus, 0x29, 0x07, absoluteGc);
  } else if (factoryMode) {
    // Legacy factory calls retain their B/W bank; typed direct passes return above.
    loadBankCdi(bus, 0x29, 0x07, _cfg.full);
  } else {
    loadBankCdi(bus, 0x29, 0x07, _cfg.gc);  // 5-frame 4-level nudge bank
  }
  triggerRefresh(bus, turnOff);
  if (!turnOff && _isScreenOn) {
    const uint8_t vcomIdle = x3_lut16::kVcomDcIdle;
    bus.cmdData(CMD_VCOM_DC, &vcomIdle, 1);
    _lastActivityMs = static_cast<uint32_t>(millis());
  }

  _hasPresentedSinceInit = true;
  _redRamSynced = false;
  _gray16CleanupActive = false;
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
  _grayState.lsbValid = false;
  _absoluteInput = false;
  _directGrayPass = false;
}

void Uc8253X3Driver::displayGray16Pass1(EpdBus& bus) { (void)displayGray16Pass1Impl(bus, /*allowDefer=*/false); }

bool Uc8253X3Driver::displayGray16Pass1Start(EpdBus& bus) { return displayGray16Pass1Impl(bus, /*allowDefer=*/true); }

bool Uc8253X3Driver::displayGray16Pass1Impl(EpdBus& bus, bool allowDefer) {
  drainPendingRefresh(bus);
  if (!supportsGray16Planes() || !_grayState.lsbValid) return false;
  const uint8_t pllPass1 = static_cast<uint8_t>(_cfg.lut16.pllPass1 & 0x0Fu);
  const uint8_t vcomPass1 = static_cast<uint8_t>(_cfg.lut16.vcomPass1 & 0x7Fu);
  const uint8_t pllStd = x3_lut16::kPllStandard;
  const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
  bus.cmdData(CMD_PLL_CONTROL, &pllPass1, 1);
  bus.cmdData(CMD_VCOM_DC, &vcomPass1, 1);
  loadBankCdi(bus, 0xA9, 0x07, _cfg.lut16.pass1Coarse);
  _inGrayscaleMode = true;
  _redRamSynced = false;
  _gray16CleanupActive = false;
  _grayState.lsbValid = false;
  if (allowDefer) {
    startAsyncRefresh(bus, PendingDrf::Gray16Pass1);
    return true;
  }
  triggerRefresh(bus, /*turnOff=*/false);
  bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
  bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
  if (_isScreenOn) _lastActivityMs = static_cast<uint32_t>(millis());
  return false;
}

void Uc8253X3Driver::displayGray16Pass2(EpdBus& bus, bool turnOff) {
  (void)displayGray16Pass2Impl(bus, turnOff, /*allowDefer=*/false);
}

bool Uc8253X3Driver::displayGray16Pass2Start(EpdBus& bus, bool turnOff) {
  return displayGray16Pass2Impl(bus, turnOff, /*allowDefer=*/true);
}

bool Uc8253X3Driver::displayGray16Pass2Impl(EpdBus& bus, bool turnOff, bool allowDefer) {
  drainPendingRefresh(bus);
  if (!supportsGray16Planes() || !_grayState.lsbValid) return false;
  const uint8_t pllPass2 = static_cast<uint8_t>(_cfg.lut16.pllPass2 & 0x0Fu);
  const uint8_t vdlPass2 = static_cast<uint8_t>(_cfg.lut16.vdlPass2 & 0x3Fu);
  const uint8_t vcomPass2 = static_cast<uint8_t>(_cfg.lut16.vcomPass2 & 0x7Fu);
  const uint8_t pwrPass2[5] = {0x07, 0x17, 0x3F, vdlPass2, 0x17};
  bus.cmdData(CMD_PLL_CONTROL, &pllPass2, 1);
  bus.cmdData(CMD_POWER_SETTING, pwrPass2, sizeof(pwrPass2));
  bus.cmdData(CMD_VCOM_DC, &vcomPass2, 1);
  loadBankCdi(bus, 0xA9, 0x07, _cfg.lut16.pass2Fine);

  _inGrayscaleMode = true;
  _redRamSynced = false;
  _gray16CleanupActive = false;
  _grayState.lsbValid = false;
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
  _absoluteInput = false;
  _directGrayPass = false;

  if (allowDefer) {
    startAsyncRefresh(bus, turnOff ? PendingDrf::Gray16Pass2TurnOff : PendingDrf::Gray16Pass2);
    return true;
  }

  triggerRefresh(bus, /*turnOff=*/false);

  // Restore standard 80 Hz PLL and +/-15.0V rails; park VCOM_DC at 0x00 during warm PON hold
  // (or restore -1.90V VCOM_DC before immediate CMD_POWER_OFF).
  const uint8_t pllStd = x3_lut16::kPllStandard;
  bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
  bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
  if (turnOff && _isScreenOn) {
    const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
    bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" X3_POF");
    _isScreenOn = false;
  } else if (_isScreenOn) {
    const uint8_t vcomIdle = x3_lut16::kVcomDcIdle;
    bus.cmdData(CMD_VCOM_DC, &vcomIdle, 1);
    _lastActivityMs = static_cast<uint32_t>(millis());
  } else {
    const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
    bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
  }
  return false;
}

void Uc8253X3Driver::beginGray16Cleanup(EpdBus& bus, const uint8_t* bw) {
  drainPendingRefresh(bus);
  _absoluteInput = false;
  _directGrayPass = false;
  _grayState.lsbValid = false;
  _gray16CleanupActive = false;
  if (!bw) {
    _redRamSynced = false;
    return;
  }
  // Restore DTM2 from Pass 2's fine MSB plane to the B/W baseline while HV rails
  // are powered on; leaving fine MSB bits set in DTM2 drifts dark tones during idle PON.
  bus.sendPlaneFlipped(CMD_DTM2, bw, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  _redRamSynced = false;
}

void Uc8253X3Driver::finishGray16Cleanup(EpdBus& bus) {
  drainPendingRefresh(bus);
  bus.cmd(CMD_DATA_STOP);
  _grayState.lsbValid = false;
  _inGrayscaleMode = false;
  _redRamSynced = true;
  _gray16CleanupActive = supportsGray16Planes();
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
  if (_isScreenOn) {
    _lastActivityMs = static_cast<uint32_t>(millis());
  }
}

void Uc8253X3Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  drainPendingRefresh(bus);
  if (_directGrayPass || _directGrayOnPanel) {
    _absoluteInput = false;
    _directGrayPass = false;
    _grayState = {};
    // A B/W shadow cannot describe the physical gray page for a differential
    // update.
    _redRamSynced = false;
    return;
  }
  _absoluteInput = false;
  _directGrayPass = false;
  if (!bw) {
    _grayState.lsbValid = false;
    _redRamSynced = false;
    return;
  }
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
  _gray16CleanupActive = false;
  _forceFullSyncNext = false;
  _forcedConditionPassesNext = 0;
  _inGrayscaleMode = false;
  if (_isScreenOn) {
    _lastActivityMs = static_cast<uint32_t>(millis());
  }
}

void Uc8253X3Driver::grayscaleRevert(EpdBus& bus, const uint8_t* fb) {
  drainPendingRefresh(bus);
  (void)fb;
  if (!_inGrayscaleMode) return;
  _inGrayscaleMode = false;
  const uint8_t pllStd = x3_lut16::kPllStandard;
  const uint8_t vcomStd = x3_lut16::kVcomDcStandard;
  bus.cmdData(CMD_PLL_CONTROL, &pllStd, 1);
  bus.cmdData(CMD_POWER_SETTING, x3_lut16::kPwrStandard, sizeof(x3_lut16::kPwrStandard));
  bus.cmdData(CMD_VCOM_DC, &vcomStd, 1);
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
  _gray16CleanupActive = false;
}

void Uc8253X3Driver::requestResync(uint8_t settlePasses) {
  _grayState = {};
  _absoluteInput = false;
  _directGrayPass = false;
  _gray16CleanupActive = false;
  _forceFullSyncNext = true;
  _forcedConditionPassesNext = settlePasses;
}

void Uc8253X3Driver::skipInitialResync() {
  _initialFullSyncsRemaining = 0;
  _redRamSynced = true;
}

void Uc8253X3Driver::setBackgroundHint(bool darkBackground) {
  if (_darkBackground == darkBackground) return;
  _darkBackground = darkBackground;
  _polarityDirty = true;
  _gray16CleanupActive = false;
}

void Uc8253X3Driver::powerOffBetweenFrames(EpdBus& bus) {
  drainPendingRefresh(bus);
  if (!_isScreenOn) return;
  bus.cmd(CMD_POWER_OFF);
  bus.waitBusy(" X3_POF");
  _isScreenOn = false;
}

bool Uc8253X3Driver::prewarmPowerOn(EpdBus& bus) {
  if (_isScreenOn || _powerOnPending || _pendingDrf != PendingDrf::None) return false;
  bus.cmd(CMD_POWER_ON);
  _powerOnPending = true;
  _powerOnStartMs = static_cast<uint32_t>(millis());
  const int8_t busyPin = bus.pins().busy;
  if (busyPin >= 0) {
    for (int i = 0; i < 20 && digitalRead(busyPin) == HIGH; ++i) {
      delayMicroseconds(10);
    }
    _powerOnSawLow = (digitalRead(busyPin) == LOW);
  } else {
    _powerOnSawLow = false;
  }
  return true;
}

bool Uc8253X3Driver::serviceIdlePower(EpdBus& bus) {
  if ((!_isScreenOn && !_powerOnPending) || _pendingDrf != PendingDrf::None) return false;
  const uint32_t now = static_cast<uint32_t>(millis());
  if (_powerOnPending) {
    if (now - _powerOnStartMs < kIdlePowerOffMs) return false;
    finishPendingPowerOn(bus);
  } else if (now - _lastActivityMs < kIdlePowerOffMs) {
    return false;
  }
  bus.cmd(CMD_POWER_OFF);
  bus.waitBusy(" X3_IDLE_POF");
  _isScreenOn = false;
  return true;
}

void Uc8253X3Driver::deepSleep(EpdBus& bus) {
  drainPendingRefresh(bus);
  _grayState = {};
  _absoluteInput = false;
  _directGrayPass = false;
  _gray16CleanupActive = false;
  _hasPresentedSinceInit = false;
  _powerOnPending = false;
  _powerOnSawLow = false;
  _pendingDrfSawLow = false;
  _lastActivityMs = 0;
  _powerOnStartMs = 0;
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
