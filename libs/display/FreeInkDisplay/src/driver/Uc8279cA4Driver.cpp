#include "Uc8279cA4Driver.h"

#include <BoardConfig.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "../lut/Uc8279cA4Luts.h"

namespace freeink {
namespace {
constexpr uint8_t CMD_PANEL_SETTING = 0x00;
constexpr uint8_t CMD_POWER_SETTING = 0x01;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_POWER_OFF_SEQUENCE = 0x03;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint8_t CMD_DTM1 = 0x10;
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;
constexpr uint8_t CMD_DTM2 = 0x13;
constexpr uint8_t CMD_LUT_VCOM = 0x20;
constexpr uint8_t CMD_PLL_CONTROL = 0x30;
constexpr uint8_t CMD_RESOLUTION = 0x61;
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;
constexpr uint8_t CMD_GATE_SOURCE_SETTING = 0x65;
constexpr uint8_t CMD_VCOM_DC = 0x82;
constexpr uint8_t CMD_TCON = 0xe1;

constexpr uint16_t CONTROLLER_HEIGHT = 600;
constexpr uint16_t PADDING_ROWS = 48;
// The fast waveform is intentionally light and accumulates visible residue on
// this panel. Cap every run at four fast paints; the fifth uses the full bank.
constexpr uint8_t MAX_CONSECUTIVE_FAST_REFRESHES = 4;
static_assert(CONTROLLER_HEIGHT - PADDING_ROWS == 552, "EEGO A4 framebuffer/padding mismatch");
}  // namespace

Uc8279cA4Driver::Uc8279cA4Driver()
    : _width(BoardConfig::ACTIVE.displayWidth),
      _height(BoardConfig::ACTIVE.displayHeight),
      _widthBytes(BoardConfig::ACTIVE.displayWidth / 8) {}

uint32_t Uc8279cA4Driver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 20000000;
}

PanelGeometry Uc8279cA4Driver::geometry() const {
  return {_width, _height, _widthBytes, static_cast<uint32_t>(_widthBytes) * _height};
}

void Uc8279cA4Driver::hardwareReset(EpdBus& bus) {
  // EpdBus's generic 2-ms low pulse is too short for this panel.
  const int8_t rst = bus.pins().rst;
  digitalWrite(rst, HIGH);
  delay(20);
  digitalWrite(rst, LOW);
  delay(10);
  digitalWrite(rst, HIGH);
  delay(100);
}

void Uc8279cA4Driver::initController(EpdBus& bus, const bool grayMode) {
  const uint8_t panel[] = {0x3f, 0x4a};
  // Full drive for both B/W and gray (PWR byte0 0x43, VCOM_DC 0x24). The OEM gray
  // path uses these same strong settings; the fork's reduced gray values
  // (0x03 / 0x20) were the cause of washed-out grays. grayMode still selects the
  // PON/white-seed ordering below.
  const uint8_t power[] = {0x43, 0x00, 0x78, 0x78, 0x17};
  const uint8_t booster[] = {0x25, 0x25, 0x3c};
  const uint8_t resolution[] = {0x03, 0x00, 0x02, 0x58};  // 768x600
  const uint8_t interval[] = {0x00, 0x00, 0x20, 0x00};
  const uint8_t powerOffSequence = 0x20;
  const uint8_t vcom = 0x24;
  const uint8_t pll = 0x0f;
  const uint8_t tcon = 0x02;

  bus.cmdData(CMD_PANEL_SETTING, panel, sizeof(panel));
  bus.cmdData(CMD_POWER_OFF_SEQUENCE, &powerOffSequence, 1);
  bus.cmdData(CMD_POWER_SETTING, power, sizeof(power));
  bus.cmdData(CMD_BOOSTER_SOFT_START, booster, sizeof(booster));
  bus.cmdData(CMD_VCOM_DC, &vcom, 1);
  bus.cmdData(CMD_PLL_CONTROL, &pll, 1);
  bus.cmdData(CMD_RESOLUTION, resolution, sizeof(resolution));
  bus.cmdData(CMD_GATE_SOURCE_SETTING, interval, sizeof(interval));
  bus.cmdData(CMD_TCON, &tcon, 1);

  if (grayMode) {
    // Four-gray drive uses a lower power setup and powers the panel before
    // seeding DTM1. Preserve that order exactly.
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" A4 gray power-on");
    fillControllerRam(bus, CMD_DTM1, 0xff);
  } else {
    // Official 1.2.7 seeds the old plane before enabling panel power.
    fillControllerRam(bus, CMD_DTM1, 0xff);
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" A4 power-on");
  }
  _screenOn = true;
  _grayControllerMode = grayMode;
}

void Uc8279cA4Driver::ensurePowerOn(EpdBus& bus) {
  if (_screenOn) return;
  bus.cmd(CMD_POWER_ON);
  bus.waitBusy(" A4 power-on");
  _screenOn = true;
}

void Uc8279cA4Driver::loadFullLut(EpdBus& bus) {
  const uint8_t* const lut[] = {A4_UC8279C_LUT_20, A4_UC8279C_LUT_21, A4_UC8279C_LUT_22, A4_UC8279C_LUT_23,
                                A4_UC8279C_LUT_24};
  for (uint8_t i = 0; i < 5; ++i) {
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), lut[i], A4_UC8279C_LUT_LENGTH);
  }
}

void Uc8279cA4Driver::loadFastLut(EpdBus& bus) {
  for (uint8_t i = 0; i < 5; ++i) {
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), A4_UC8279C_FAST_LUT + i * A4_UC8279C_FAST_RECORD_LENGTH,
                A4_UC8279C_FAST_WRITE_LENGTH);
  }
}

void Uc8279cA4Driver::loadGrayLut(EpdBus& bus) {
  for (uint8_t i = 0; i < 5; ++i) {
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), A4_UC8279C_GRAY_LUT + i * A4_UC8279C_GRAY_RECORD_LENGTH,
                A4_UC8279C_GRAY_RECORD_LENGTH);
  }
}

void Uc8279cA4Driver::writeFrame(EpdBus& bus, const uint8_t command, const uint8_t* fb, const bool invert) {
  uint8_t white[96];
  memset(white, 0xff, sizeof(white));
  uint8_t rowBuf[96];
  bus.cmd(command);
  bus.beginTxn();
  for (int y = static_cast<int>(_height) - 1; y >= 0; --y) {
    const uint8_t* row = fb + static_cast<uint32_t>(y) * _widthBytes;
    if (invert) {
      for (uint16_t xb = 0; xb < _widthBytes; ++xb) rowBuf[xb] = static_cast<uint8_t>(~row[xb]);
      row = rowBuf;
    }
    bus.rawWriteBytes(row, _widthBytes);
  }
  for (uint16_t y = 0; y < PADDING_ROWS; ++y) {
    bus.rawWriteBytes(white, _widthBytes);
  }
  bus.endTxn();
}

void Uc8279cA4Driver::fillControllerRam(EpdBus& bus, const uint8_t command, const uint8_t fill) {
  bus.fillPlane(command, fill, CONTROLLER_HEIGHT, _widthBytes);
}

void Uc8279cA4Driver::refresh(EpdBus& bus, const bool turnOff) {
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" A4 refresh");
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" A4 power-off");
    _screenOn = false;
  }
}

void Uc8279cA4Driver::begin(EpdBus& bus) {
  _fastRefreshesSinceFull = 0;
  hardwareReset(bus);
  initController(bus);
  _firstRefreshPending = true;
}

void Uc8279cA4Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, const RefreshMode mode,
                              const bool turnOff) {
  (void)prev;
  // Coming out of gray mode: the AA overlay leaves gray edge charge a plain fast
  // (DU) diff can't scrub, so it ghosts under page turns. Re-init and force a full
  // (GC) refresh once to re-drive every pixel from a clean baseline (same idea as
  // the UC8279 X4 driver's post-gray re-drive).
  const bool scrubAfterGray = _grayControllerMode || _redriveAfterGray;
  if (_grayControllerMode) {
    hardwareReset(bus);
    initController(bus, false);
  }
  _redriveAfterGray = false;
  ensurePowerOn(bus);
  bool fast = mode == RefreshMode::Fast;
  if (fast && !_holdPeriodicFull && _fastRefreshesSinceFull >= MAX_CONSECUTIVE_FAST_REFRESHES) fast = false;
  if (scrubAfterGray || _firstRefreshPending) fast = false;
  if (fast) {
    ++_fastRefreshesSinceFull;
  } else {
    _fastRefreshesSinceFull = 0;
  }
  const uint8_t interval = fast ? 0xd7 : 0x97;
  bus.cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);
  if (fast) {
    // Fast mode sends the first 42 bytes of each 49-byte record.
    loadFastLut(bus);
  } else {
    // HALF is CrossPoint's periodic ghost-cleanup cadence on this target.
    // Treat it as a complete refresh.
    loadFullLut(bus);
  }
  if (_firstRefreshPending) {
    // First refresh after begin(): the glass state is unknown (reflash/reset),
    // so seed the old plane with the inverse of this frame — every pixel
    // transitions and the full LUT drives the whole panel to a clean baseline.
    writeFrame(bus, CMD_DTM1, fb, /*invert=*/true);
    _firstRefreshPending = false;
  }
  writeFrame(bus, CMD_DTM2, fb);
  refresh(bus, turnOff);
}

uint8_t* Uc8279cA4Driver::allocateGrayBuffer() {
  const size_t bytes = static_cast<size_t>(_widthBytes) * _height;
  uint8_t* result = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!result) result = static_cast<uint8_t*>(malloc(bytes));
  return result;
}

void Uc8279cA4Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (!lsb) return;
  if (!_grayLsb) _grayLsb = allocateGrayBuffer();
  if (_grayLsb) {
    // The A4 gray LUT maps a set bit to the lighter level (keeps the 0xff seed)
    // while a cleared bit drives the pixel darker. The BW framebuffer uses the
    // opposite polarity (set bit == dark text), so complement before storing to
    // render black text on a light background instead of an inverted black page.
    const size_t n = static_cast<size_t>(_widthBytes) * _height;
    for (size_t i = 0; i < n; ++i) _grayLsb[i] = static_cast<uint8_t>(~lsb[i]);
  }
}

void Uc8279cA4Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  if (!msb) return;
  if (!_grayMsb) _grayMsb = allocateGrayBuffer();
  if (_grayMsb) {
    const size_t n = static_cast<size_t>(_widthBytes) * _height;
    for (size_t i = 0; i < n; ++i) _grayMsb[i] = static_cast<uint8_t>(~msb[i]);
  }
}

void Uc8279cA4Driver::displayGray(EpdBus& bus, const uint8_t* fb, const bool turnOff, const unsigned char* lut,
                                  const bool factoryMode) {
  (void)fb;
  (void)lut;
  (void)factoryMode;
  // Allocation failure means the already-painted BW page remains visible;
  // never substitute the last rendered bit-plane as a black/white frame.
  if (!_grayLsb || !_grayMsb) return;

  hardwareReset(bus);
  initController(bus, true);
  // LSB -> DTM1 (old, 0x10), MSB -> DTM2 (new, 0x13) — the plane order both proven
  // UC8279 drivers (Uc8279Driver / Uc8279X4Driver) use with the standard 0x21-0x24
  // transition LUT. The fork had these swapped, scrambling the four gray levels.
  writeFrame(bus, CMD_DTM1, _grayLsb);
  writeFrame(bus, CMD_DTM2, _grayMsb);
  // Both planes are fully written here and _redriveAfterGray already forces the
  // next B/W refresh to a full drive, so the first-refresh inverse seed is moot.
  _firstRefreshPending = false;
  loadGrayLut(bus);
  refresh(bus, turnOff);
  _fastRefreshesSinceFull = 0;
  // The next B/W refresh must re-drive from a clean baseline to scrub gray residue.
  _redriveAfterGray = true;
}

void Uc8279cA4Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw || !_screenOn) return;
  // This keeps both controller planes coherent for callers that explicitly
  // request cleanup. The next normal display also leaves gray drive mode via
  // a complete controller re-init before applying the fast waveform.
  writeFrame(bus, CMD_DTM1, bw);
  writeFrame(bus, CMD_DTM2, bw);
}

void Uc8279cA4Driver::deepSleep(EpdBus& bus) {
  _fastRefreshesSinceFull = 0;
  if (_screenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" A4 power-off");
    _screenOn = false;
  }
  const uint8_t check = 0xa5;
  bus.cmdData(CMD_DEEP_SLEEP, &check, 1);
}

PanelDriver& uc8279cA4Driver() {
  static Uc8279cA4Driver instance;
  return instance;
}

}  // namespace freeink
