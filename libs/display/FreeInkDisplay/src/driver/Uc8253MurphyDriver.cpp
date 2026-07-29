#include "Uc8253MurphyDriver.h"

#include <BoardConfig.h>

#include "../lut/Uc8253MurphyLuts.h"

// OEM alternate/partial refresh package for RefreshMode::Fast: data-driven init
// variant + ALT LUT bank + 0x17/0xA5 trigger (see display() below). Default ON;
// build with -DFREEINK_MURPHY_OEM_PARTIAL=0 to restore the previous synthetic
// destination-drive FAST bank under the mode-0 init if the port misbehaves.
#ifndef FREEINK_MURPHY_OEM_PARTIAL
#define FREEINK_MURPHY_OEM_PARTIAL 1
#endif

namespace freeink {
namespace {
// UC8253 command set (shared with the X3 panel; Murphy uses a different init,
// resolution, and waveform set, and writes RAM 90°-rotated).
constexpr uint8_t CMD_PANEL_SETTING = 0x00;
constexpr uint8_t CMD_POWER_SETTING = 0x01;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint8_t CMD_DTM1 = 0x10;
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
constexpr uint8_t CMD_VCOM_DC = 0x82;

// Controller-native geometry: the UC8253 sees a 240x416 portrait panel. The
// device is held with that panel rotated 90° relative to the UI, so the facade's
// framebuffer is landscape 416x240 and writePlane() rotates each plane into the
// controller's 240x416 RAM (30 bytes per controller row, 416 rows).
constexpr uint16_t CTRL_W = 240;
constexpr uint16_t CTRL_H = 416;
constexpr uint16_t CTRL_WB = CTRL_W / 8;  // 30

#if FREEINK_MURPHY_OEM_PARTIAL
// OEM auto-refresh trigger used by the alternate/partial mode instead of 0x12.
// Ghidra FUN_42038f74/FUN_42038fa4 else-branch: FUN_420384e8(obj, 0x17, 0xA5) —
// command 0x17 with the single data byte 0xA5, then a BUSY wait. No window
// coordinate bytes exist anywhere in the decompiled trigger or init path; the
// refresh covers whatever the DTM planes hold (we always write the full
// 240x416 frame). On the UC8151/UC8253 family 0x17 is the AUTO register and
// 0xA5 runs the PON -> DRF -> POF sequence internally — consistent with the
// OEM alt init never issuing 0x04 (power on) itself.
constexpr uint8_t CMD_AUTO_SEQUENCE = 0x17;
constexpr uint8_t AUTO_PON_DRF_POF = 0xA5;

// ALT bank register mapping, per the Ghidra LUT loader FUN_42038b60 else
// branch. The loader keys on a one-shot RTC flag (boot value 0, cleared again
// after any use), so the boot-default steady state loads:
//   0x20 <- MURPHY_LUT_20_ALT   (56 B)   0x21 <- MURPHY_LUT_21_ALT   (42 B)
//   0x22 <- MURPHY_LUT_23_ALT_B (56 B)   0x23 <- MURPHY_LUT_22_ALT_A (42 B)
//   0x24 <- MURPHY_LUT_24_ALT   (42 B)
// i.e. the "22_A"/"23_B" array names from the findings doc are CROSSED over
// registers 0x22/0x23 here. That steady-state mapping is also the physically
// consistent one under the doc's voltage-code decode (0x8F = VSL/to-white bit,
// 0x4F = VSH/to-black bit): BW (0x22) ends on a to-white kick (0f 8f 0f) and
// WB (0x23) on a to-black kick (4f 8f 4f).
// FIXME(doc ambiguity): with the one-shot flag SET the OEM loads the swapped
// arrangement (0x22 <- 22_ALT_A @42, 0x23 <- 23_ALT_B @56) for exactly one
// refresh — the arrangement the findings-doc array names suggest. No writer of
// that flag appears in the reverse-engineering docs, so the swap's purpose is
// unknown; we ship the boot-default steady-state mapping only.
constexpr Uc8253MurphyLutBank ALT_BANK = {
    MURPHY_LUT_20_ALT,    // vcom 0x20
    MURPHY_LUT_21_ALT,    // ww   0x21
    MURPHY_LUT_23_ALT_B,  // bw   0x22  (56 B — see mapping note above)
    MURPHY_LUT_22_ALT_A,  // wb   0x23  (42 B)
    MURPHY_LUT_24_ALT,    // bb   0x24
};
constexpr Uc8253MurphyLutLens ALT_LENS = {
    sizeof(MURPHY_LUT_20_ALT),    // 56 (loader writes 0x38)
    sizeof(MURPHY_LUT_21_ALT),    // 42 (0x2a)
    sizeof(MURPHY_LUT_23_ALT_B),  // 56 (0x38)
    sizeof(MURPHY_LUT_22_ALT_A),  // 42 (0x2a)
    sizeof(MURPHY_LUT_24_ALT),    // 42 (0x2a)
};
#endif  // FREEINK_MURPHY_OEM_PARTIAL
}  // namespace

const Uc8253MurphyConfig& uc8253MurphyDefaultConfig() {
  static const Uc8253MurphyConfig cfg = {
      {MURPHY_LUT_20_DEFAULT, MURPHY_LUT_21_DEFAULT, MURPHY_LUT_22_DEFAULT, MURPHY_LUT_23_DEFAULT, MURPHY_LUT_24_DEFAULT},
      {MURPHY_LUT_20_FAST, MURPHY_LUT_21_FAST, MURPHY_LUT_22_FAST, MURPHY_LUT_23_FAST, MURPHY_LUT_24_FAST},
      {MURPHY_LUT_LEN_VCOM, MURPHY_LUT_LEN_WW, MURPHY_LUT_LEN_BW, MURPHY_LUT_LEN_WB, MURPHY_LUT_LEN_BB},  // 42 each (OEM writes ten 42-byte payloads)
      // Non-flashing FAST refreshes are allowed to run this many times in a row
      // before one is promoted to the OEM three-phase GC bank (the inversion
      // flash that clears accumulated residue). Applies to both FAST flavors:
      // the OEM partial kicks (FREEINK_MURPHY_OEM_PARTIAL, single short kick
      // per pixel) and the synthetic destination-drive bank (flag=0) are
      // DC-unbalanced per refresh, so the cadence is the anti-ghosting story:
      // lower = cleaner, higher = fewer flashes. 4 after hardware feedback
      // (6 left a distracting residue while reading).
      // (Interval semantics: N fast refreshes pass, the N+1th promotes.)
      4,
  };
  return cfg;
}

Uc8253MurphyDriver::Uc8253MurphyDriver(const Uc8253MurphyConfig& cfg)
    : _cfg(cfg),
      _fbW(BoardConfig::ACTIVE.displayWidth),
      _fbH(BoardConfig::ACTIVE.displayHeight),
      _fbWb(BoardConfig::ACTIVE.displayWidth / 8) {}

uint32_t Uc8253MurphyDriver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 4000000;
}

PanelGeometry Uc8253MurphyDriver::geometry() const {
  return {_fbW, _fbH, _fbWb, static_cast<uint32_t>(_fbWb) * _fbH};
}

void Uc8253MurphyDriver::loadLut(EpdBus& bus, const Uc8253MurphyLutBank& bank, const Uc8253MurphyLutLens& lens) {
  bus.cmdData(CMD_LUT_VCOM, bank.vcom, lens.vcom);
  bus.cmdData(CMD_LUT_WW, bank.ww, lens.ww);
  bus.cmdData(CMD_LUT_BW, bank.bw, lens.bw);
  bus.cmdData(CMD_LUT_WB, bank.wb, lens.wb);
  bus.cmdData(CMD_LUT_BB, bank.bb, lens.bb);
}

// Rotate the landscape framebuffer (416x240) into the controller's portrait RAM
// (240x416). Controller pixel (cx,cy) maps to framebuffer (srcX=cy, srcY=fbH-1-cx).
void Uc8253MurphyDriver::writePlane(EpdBus& bus, uint8_t command, const uint8_t* fb) {
  bus.cmd(command);
  bus.beginTxn();
  uint8_t row[CTRL_WB];
  for (uint16_t cy = 0; cy < CTRL_H; cy++) {
    const uint16_t srcX = cy;  // 0..415 -> framebuffer column
    for (uint16_t b = 0; b < CTRL_WB; b++) row[b] = 0;
    for (uint16_t cx = 0; cx < CTRL_W; cx++) {
      const uint16_t srcY = static_cast<uint16_t>((_fbH - 1) - cx);  // 239..0 -> framebuffer row
      const uint8_t bit = (fb[srcY * _fbWb + (srcX >> 3)] >> (7 - (srcX & 7))) & 0x01;
      if (bit) row[cx >> 3] |= static_cast<uint8_t>(1 << (7 - (cx & 7)));
    }
    bus.rawWriteBytes(row, CTRL_WB);
  }
  bus.endTxn();
}

void Uc8253MurphyDriver::fillPlane(EpdBus& bus, uint8_t command, uint8_t fillByte) {
  bus.fillPlane(command, fillByte, CTRL_H, CTRL_WB);
}

void Uc8253MurphyDriver::triggerRefresh(EpdBus& bus, bool turnOff) {
  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" M3_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" M3_DRF");
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" M3_POF");
    _isScreenOn = false;
  }
}

void Uc8253MurphyDriver::initController(EpdBus& bus) {
  bus.cmd(CMD_POWER_SETTING);
  bus.data(0x03);
  bus.data(0x10);
  bus.data(0x3F);
  bus.data(0x3B);
  bus.data(0x0D);
  bus.cmd(CMD_BOOSTER_SOFT_START);
  bus.data(0xD7);
  bus.data(0xD7);
  bus.data(0x1F);
  bus.cmd(CMD_POWER_ON);
  bus.waitBusy(" M3_PON");
  _isScreenOn = true;
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(0xFF);
  bus.cmd(CMD_PLL_CONTROL);
  bus.data(0x09);
  bus.cmd(CMD_RESOLUTION);
  bus.data(0xF0);  // 240 wide
  bus.data(0x01);  // 0x01A0 = 416 tall
  bus.data(0xA0);
  bus.cmd(CMD_VCOM_DC);
  bus.data(0x0F);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(0x97);

}

#if FREEINK_MURPHY_OEM_PARTIAL
// OEM data-driven init variant — Ghidra FUN_420389ec else-branch, byte-exact,
// in OEM command order (0x00, 0x01, 0x06, 0x30, 0x82, 0x61, 0x50). The single
// data bytes 0x30=0x09, 0x82=0x07, 0x50=0xD7 are immediate in the decompile;
// the 0x00/0x01/0x06/0x61 payloads are pointer-fed (lengths 2/5/3/3). The
// findings doc spells out only the 0x01 payload ("03 10 3F 3F 03", extracted
// at 0x3c236ca3 — display_driver.md line 93-97); the other three were read
// from the doc's cited device dump (app0_seg0_3c190020.bin) by resolving the
// literal pool FUN_420389ec loads from (seg3+0x2274..0x2280): the pointers are
// contiguous — 0x00 @0x3c236ca1 = FF 01, 0x01 @0x3c236ca3 = 03 10 3F 3F 03
// (matches the doc byte-for-byte, anchoring the extraction), 0x06 @0x3c236ca8
// = 17 37 3D, 0x61 @0x3c236cab = F0 01 A0. The same 13-byte block appears
// verbatim in the upstream OEM touch v525 firmware at file offset 0xab5e1.
// Unlike the mode-0 init there is NO power-on (0x04) here: the 0x17/0xA5 auto
// trigger powers the panel itself (see triggerRefreshAlt).
void Uc8253MurphyDriver::initControllerAlt(EpdBus& bus) {
  static constexpr uint8_t PSR_ALT[2] = {0xFF, 0x01};
  static constexpr uint8_t PWR_ALT[5] = {0x03, 0x10, 0x3F, 0x3F, 0x03};
  static constexpr uint8_t BTST_ALT[3] = {0x17, 0x37, 0x3D};
  static constexpr uint8_t RES_ALT[3] = {0xF0, 0x01, 0xA0};  // 240 x 416, same as mode-0
  bus.cmdData(CMD_PANEL_SETTING, PSR_ALT, sizeof(PSR_ALT));
  bus.cmdData(CMD_POWER_SETTING, PWR_ALT, sizeof(PWR_ALT));
  bus.cmdData(CMD_BOOSTER_SOFT_START, BTST_ALT, sizeof(BTST_ALT));
  bus.cmd(CMD_PLL_CONTROL);
  bus.data(0x09);
  bus.cmd(CMD_VCOM_DC);
  bus.data(0x07);
  bus.cmdData(CMD_RESOLUTION, RES_ALT, sizeof(RES_ALT));
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(0xD7);
}

// OEM alternate refresh trigger: 0x17 with data 0xA5, then BUSY wait (Ghidra
// FUN_42038f74 else-branch). 0xA5 auto-sequences PON -> DRF -> POF inside the
// controller, so no explicit 0x04 before or 0x02 after — matching the OEM,
// whose power-off helper is a no-op in this mode (its powered flag is only set
// by an explicit 0x04). The panel is therefore always off after this returns.
void Uc8253MurphyDriver::triggerRefreshAlt(EpdBus& bus) {
  bus.cmd(CMD_AUTO_SEQUENCE);
  bus.data(AUTO_PON_DRF_POF);
  bus.waitBusy(" M3_AUTO");
  _isScreenOn = false;
}
#endif  // FREEINK_MURPHY_OEM_PARTIAL

void Uc8253MurphyDriver::begin(EpdBus& bus) {
  bus.reset(200);  // Murphy panel wants a long post-reset settle
  _isScreenOn = false;
  _fastRefreshCount = 0;
  initController(bus);
}

void Uc8253MurphyDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  // Manufacturer guidance: hardware-reset and re-initialise the controller before
  // every refresh. The UC8253 retains LUT/RAM state between refreshes, and reusing
  // it leaves pixels half-latched (the previous frame bleeds through / flashes
  // without settling). A fresh reset+init each time clears that residue.
  bus.reset(200);
  _isScreenOn = false;

  // FAST ghosts over time (both the OEM partial kicks and the synthetic DU
  // bank), so promote to a full (GC) refresh every ghostClearInterval refreshes.
  bool useFast = (mode == RefreshMode::Fast);
  if (useFast) {
    if (_cfg.ghostClearInterval != 0 && _fastRefreshCount >= _cfg.ghostClearInterval) {
      useFast = false;
      _fastRefreshCount = 0;
    } else {
      _fastRefreshCount++;
    }
  } else {
    _fastRefreshCount = 0;
  }

#if FREEINK_MURPHY_OEM_PARTIAL
  if (useFast) {
    // OEM alternate/partial package: data-driven init + ALT bank + 0x17/0xA5.
    // This path is differential — the ALT bank's BW/WB kicks exist to fire on
    // DTM1 != DTM2 (display_driver.md: "LUTBW/LUTWB exist for the alternate
    // 0x17/0xA5 partial-refresh path and only do a short kick"): old frame ->
    // DTM1, new frame -> DTM2, OEM plane order (0x10 then 0x13). On
    // single-buffer builds the facade passes prev == nullptr; fall back to the
    // same buffer in both planes, which degrades gracefully to the gentle
    // same-state WW/BB kicks only (changed pixels then rely on the next
    // full/promoted refresh — nothing mis-drives).
    initControllerAlt(bus);
    loadLut(bus, ALT_BANK, ALT_LENS);
    writePlane(bus, CMD_DTM1, prev != nullptr ? prev : fb);
    writePlane(bus, CMD_DTM2, fb);
    // 0xA5 auto-sequences PON -> DRF -> POF, so the panel ends powered off no
    // matter what turnOff asked for; the next display() re-inits from reset
    // anyway, so an unused-on flag is never left dangling.
    (void)turnOff;
    triggerRefreshAlt(bus);
    return;
  }
#endif

  initController(bus);
  loadLut(bus, useFast ? _cfg.fast : _cfg.def, _cfg.lens);

  // OEM full-refresh scheme: the SAME buffer goes to both planes, always — only
  // WW/BB fire and every pixel is driven straight to its target. A real
  // differential (old->DTM1, new->DTM2) through the DEFAULT/FAST LUTs leaves
  // pixels half-flipped (verified empirically, see the Murphy repo display
  // findings), so it is deliberately not attempted even when a previous frame
  // exists.
  (void)prev;
  writePlane(bus, CMD_DTM1, fb);
  writePlane(bus, CMD_DTM2, fb);

  triggerRefresh(bus, turnOff);
}

void Uc8253MurphyDriver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" M3 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// Per-board waveform injection mirrors the X3 driver: a board driving a different
// Murphy-class UC8253 panel supplies its own LUT banks via
// -DFREEINK_UC8253_MURPHY_CONFIG=yourConfig without editing this driver.
#ifdef FREEINK_UC8253_MURPHY_CONFIG
const Uc8253MurphyConfig& FREEINK_UC8253_MURPHY_CONFIG();
static const Uc8253MurphyConfig& uc8253MurphyActiveConfig() { return FREEINK_UC8253_MURPHY_CONFIG(); }
#else
static const Uc8253MurphyConfig& uc8253MurphyActiveConfig() { return uc8253MurphyDefaultConfig(); }
#endif

PanelDriver& uc8253MurphyDriver() {
  static Uc8253MurphyDriver instance(uc8253MurphyActiveConfig());
  return instance;
}

}  // namespace freeink
