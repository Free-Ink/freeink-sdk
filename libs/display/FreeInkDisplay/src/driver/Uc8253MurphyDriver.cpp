#include "Uc8253MurphyDriver.h"

#include <BoardConfig.h>

#include "../lut/Uc8253MurphyLuts.h"

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
}  // namespace

const Uc8253MurphyConfig& uc8253MurphyDefaultConfig() {
  static const Uc8253MurphyConfig cfg = {
      {MURPHY_LUT_20_DEFAULT, MURPHY_LUT_21_DEFAULT, MURPHY_LUT_22_DEFAULT, MURPHY_LUT_23_DEFAULT, MURPHY_LUT_24_DEFAULT},
      {MURPHY_LUT_20_FAST, MURPHY_LUT_21_FAST, MURPHY_LUT_22_FAST, MURPHY_LUT_23_FAST, MURPHY_LUT_24_FAST},
      {MURPHY_LUT_LEN_VCOM, MURPHY_LUT_LEN_WW, MURPHY_LUT_LEN_BW, MURPHY_LUT_LEN_WB, MURPHY_LUT_LEN_BB},  // 56/42/56/42/42
      8,  // promote FAST -> full every 8 refreshes
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

void Uc8253MurphyDriver::loadLut(EpdBus& bus, const Uc8253MurphyLutBank& bank) {
  bus.cmdData(CMD_LUT_VCOM, bank.vcom, _cfg.lens.vcom);
  bus.cmdData(CMD_LUT_WW, bank.ww, _cfg.lens.ww);
  bus.cmdData(CMD_LUT_BW, bank.bw, _cfg.lens.bw);
  bus.cmdData(CMD_LUT_WB, bank.wb, _cfg.lens.wb);
  bus.cmdData(CMD_LUT_BB, bank.bb, _cfg.lens.bb);
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
  initController(bus);

  // FAST (DU) ghosts over time, so promote to a full (GC) refresh every
  // ghostClearInterval refreshes.
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

  loadLut(bus, useFast ? _cfg.fast : _cfg.def);

  // Full (GC) refresh writes the new frame to BOTH planes, so only WW/BB fire and
  // every pixel is fully driven to its target — clean, no half-flipped pixels.
  // FAST (DU) refresh is differential: old frame -> DTM1, new -> DTM2, so unchanged
  // pixels take WW/BB and changed pixels take the quick BW/WB transition kicks.
  // Without a previous frame (single-buffer builds) fall back to both-planes-new.
  if (useFast && prev != nullptr) {
    writePlane(bus, CMD_DTM1, prev);
    writePlane(bus, CMD_DTM2, fb);
  } else {
    writePlane(bus, CMD_DTM1, fb);
    writePlane(bus, CMD_DTM2, fb);
  }

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
