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

void Uc8279cA4Driver::initController(EpdBus& bus, const Session session) {
  // Gray3 uses stock V1.0.3's reader session and four-gray stock V1.3.0's image session.
  // B/W keeps stronger rails because this driver changes UI screens with fast
  // refreshes, with stock's VCOM_DC 0x32.
  const bool fourGray = session == Session::FourGray;
  const uint8_t panel[] = {0x3f, 0x4a};
  const uint8_t gray3Power[] = {0x03, 0x00, 0x72, 0x72};
  const uint8_t fourGrayPower[] = {0x03, 0x00, 0x78, 0x78, 0x17};
  const uint8_t bwPower[] = {0x43, 0x00, 0x78, 0x78, 0x17};
  const uint8_t booster[] = {0x25, 0x25, 0x3c};
  const uint8_t resolution[] = {0x03, 0x00, 0x02, 0x58};  // 768x600
  const uint8_t interval[] = {0x00, 0x00, static_cast<uint8_t>(fourGray ? 0x20 : 0x00), 0x00};
  const uint8_t powerOffSequence = 0x20;
  const uint8_t vcom = fourGray ? 0x20 : 0x32;
  const uint8_t pll = 0x0f;
  const uint8_t tcon = 0x02;

  bus.cmdData(CMD_PANEL_SETTING, panel, sizeof(panel));
  bus.cmdData(CMD_POWER_OFF_SEQUENCE, &powerOffSequence, 1);
  if (session == Session::Bw)
    bus.cmdData(CMD_POWER_SETTING, bwPower, sizeof(bwPower));
  else if (fourGray)
    bus.cmdData(CMD_POWER_SETTING, fourGrayPower, sizeof(fourGrayPower));
  else
    bus.cmdData(CMD_POWER_SETTING, gray3Power, sizeof(gray3Power));
  bus.cmdData(CMD_BOOSTER_SOFT_START, booster, sizeof(booster));
  bus.cmdData(CMD_VCOM_DC, &vcom, 1);
  bus.cmdData(CMD_PLL_CONTROL, &pll, 1);
  bus.cmdData(CMD_RESOLUTION, resolution, sizeof(resolution));
  bus.cmdData(CMD_GATE_SOURCE_SETTING, interval, sizeof(interval));
  bus.cmdData(CMD_TCON, &tcon, 1);

  // Stock seeds the old plane white, then enables panel power.
  fillControllerRam(bus, CMD_DTM1, 0xff);
  bus.cmd(CMD_POWER_ON);
  bus.waitBusy(" A4 power-on");
  _screenOn = true;
  _grayControllerMode = session != Session::Bw;
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
  const uint8_t* const lut[] = {A4_UC8279C_FAST_LUT_20, A4_UC8279C_FAST_LUT_21, A4_UC8279C_FAST_LUT_22,
                                A4_UC8279C_FAST_LUT_23, A4_UC8279C_FAST_LUT_24};
  for (uint8_t i = 0; i < 5; ++i) {
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), lut[i], A4_UC8279C_LUT_LENGTH);
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
  auto txn = bus.beginTxn();
  for (int y = static_cast<int>(_height) - 1; y >= 0; --y) {
    const uint8_t* row = fb + static_cast<uint32_t>(y) * _widthBytes;
    if (invert) {
      for (uint16_t xb = 0; xb < _widthBytes; ++xb) rowBuf[xb] = static_cast<uint8_t>(~row[xb]);
      row = rowBuf;
    }
    txn.writeBytes(row, _widthBytes);
  }
  for (uint16_t y = 0; y < PADDING_ROWS; ++y) {
    txn.writeBytes(white, _widthBytes);
  }
}

void Uc8279cA4Driver::fillControllerRam(EpdBus& bus, const uint8_t command, const uint8_t fill) {
  bus.fillPlane(command, fill, CONTROLLER_HEIGHT, _widthBytes);
}

void Uc8279cA4Driver::refresh(EpdBus& bus, const uint8_t* bwBaseline) {
  ensurePowerOn(bus);
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" A4 refresh");
  // The next differential update diffs against this baseline.
  if (bwBaseline) writeFrame(bus, CMD_DTM1, bwBaseline);
  // Always power off: leaving the panel powered between refreshes made later
  // four-gray images show earlier screens as ghosts on hardware.
  bus.cmd(CMD_POWER_OFF);
  bus.waitBusy(" A4 power-off");
  _screenOn = false;
}

void Uc8279cA4Driver::begin(EpdBus& bus) {
  _redriveAfterGray = false;
  _gray3HistoryValid = false;
  _gray3Session = false;
  _basePending = false;
  hardwareReset(bus);
  initController(bus, Session::Bw);
  _firstRefreshPending = true;
}

void Uc8279cA4Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, const RefreshMode mode,
                              const bool turnOff) {
  (void)prev;
  (void)turnOff;  // refresh() always powers off
  // Any B/W refresh replaces what a Gray3 transition would start from.
  _gray3HistoryValid = false;
  _gray3Session = false;
  _basePending = false;
  // Restore B/W controller registers after a grayscale pass.
  if (_grayControllerMode) {
    hardwareReset(bus);
    initController(bus, Session::Bw);
  }
  ensurePowerOn(bus);
  const bool fast = mode == RefreshMode::Fast && !_firstRefreshPending;
  const uint8_t interval = fast ? 0xd7 : 0x97;
  bus.cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);
  if (fast) {
    // Stock's production fast (DU) waveform.
    loadFastLut(bus);
  } else {
    // HALF is CrossPoint's periodic ghost-cleanup cadence on this target.
    // Treat it as a complete refresh.
    loadFullLut(bus);
  }
  if (_firstRefreshPending) {
    // One full pass leaves the image held through sleep ghosting through the
    // first frame. Drive every pixel to the inverse first, then to the target.
    writeFrame(bus, CMD_DTM1, fb);
    writeFrame(bus, CMD_DTM2, fb, /*invert=*/true);
    refresh(bus);
  }
  if (_firstRefreshPending || _redriveAfterGray) {
    // Inverse OLD makes every target pixel transition, clearing unknown startup
    // state or residual AA charge even with the fast waveform.
    writeFrame(bus, CMD_DTM1, fb, /*invert=*/true);
    _firstRefreshPending = false;
  }
  writeFrame(bus, CMD_DTM2, fb);
  refresh(bus, fb);
  // The displayed B/W frame is the base any following overlay gray pass folds
  // onto, however the caller started that pass.
  if (!_grayBase) _grayBase = allocateGrayBuffer();
  if (_grayBase && _grayBase != fb) memcpy(_grayBase, fb, static_cast<size_t>(_widthBytes) * _height);
  _redriveAfterGray = false;
}

uint8_t* Uc8279cA4Driver::allocateGrayBuffer() {
  const size_t bytes = static_cast<size_t>(_widthBytes) * _height;
  uint8_t* result = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!result) result = static_cast<uint8_t*>(malloc(bytes));
  return result;
}

void Uc8279cA4Driver::beginGrayscale(EpdBus& bus, const uint8_t* fb, const GrayscaleMode mode,
                                     const RefreshMode fallback, const bool turnOff) {
  (void)bus;
  (void)turnOff;
  _overlayPass = mode == GrayscaleMode::Overlay;
  if (!_overlayPass) return;  // absolute planes carry every pixel: no base pass
  // Gray3 sends the base with its grays in one refresh: stash it undisplayed.
  if (!_grayBase) _grayBase = allocateGrayBuffer();
  if (!_grayBase || !fb) return;
  memcpy(_grayBase, fb, static_cast<size_t>(_widthBytes) * _height);
  _basePending = true;
  _cleanupPage = fallback != RefreshMode::Fast;
}

void Uc8279cA4Driver::writeGrayscalePlaneStrip(EpdBus& bus, const GrayPlane plane, const uint8_t* rows,
                                               const uint16_t yStart, const uint16_t numRows) {
  (void)bus;
  if (!rows || numRows == 0 || yStart + numRows > _height) return;
  uint8_t*& target = plane == GrayPlane::Lsb ? _grayLsb : _grayMsb;
  if (!target) target = allocateGrayBuffer();
  if (target) memcpy(target + static_cast<size_t>(yStart) * _widthBytes, rows, static_cast<size_t>(numRows) * _widthBytes);
}

void Uc8279cA4Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (!lsb) return;
  if (!_grayLsb) _grayLsb = allocateGrayBuffer();
  if (_grayLsb) memcpy(_grayLsb, lsb, static_cast<size_t>(_widthBytes) * _height);
}

void Uc8279cA4Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  if (!msb) return;
  if (!_grayMsb) _grayMsb = allocateGrayBuffer();
  if (_grayMsb) memcpy(_grayMsb, msb, static_cast<size_t>(_widthBytes) * _height);
}

bool Uc8279cA4Driver::allocateGray3() {
  if (!_gray3White) _gray3White = allocateGrayBuffer();
  if (!_gray3Middle) _gray3Middle = allocateGrayBuffer();
  return _gray3White && _gray3Middle;
}

// Gray3 page in _grayLsb (white mask) / _grayMsb (middle mask). Each pixel's
// code comes from its new level and the level already on the panel (stock
// V1.0.3): unchanged white/middle pixels hold, black is always re-driven, white
// after middle/black is cleared, middle after white/black is nudged gray.
void Uc8279cA4Driver::writeGray3Codes(EpdBus& bus, const bool dtm1, bool& anyBlack, bool& anyWhiteClear) {
  uint8_t rowBuf[96];
  bus.cmd(dtm1 ? CMD_DTM1 : CMD_DTM2);
  auto txn = bus.beginTxn();
  for (int y = static_cast<int>(_height) - 1; y >= 0; --y) {
    const size_t row = static_cast<size_t>(y) * _widthBytes;
    for (uint16_t xb = 0; xb < _widthBytes; ++xb) {
      const size_t i = row + xb;
      const uint8_t white = _grayLsb[i];
      const uint8_t middle = _grayMsb[i];
      const auto black = static_cast<uint8_t>(~(white | middle));
      const auto whiteClear = static_cast<uint8_t>(white & ~_gray3White[i]);
      const auto gray = static_cast<uint8_t>(middle & ~_gray3Middle[i]);
      if (black) anyBlack = true;
      if (whiteClear) anyWhiteClear = true;
      // code 10 = black, 01 = gray, 11 = white-clear, 00 = hold; DTM1 = bit 1.
      rowBuf[xb] = dtm1 ? static_cast<uint8_t>(whiteClear | black) : static_cast<uint8_t>(whiteClear | gray);
    }
    txn.writeBytes(rowBuf, _widthBytes);
  }
  memset(rowBuf, 0x00, sizeof(rowBuf));  // padding rows hold
  for (uint16_t y = 0; y < PADDING_ROWS; ++y) txn.writeBytes(rowBuf, _widthBytes);
}

void Uc8279cA4Driver::loadGray3Lut(EpdBus& bus, const bool anyBlack, const bool anyWhiteClear) {
  for (uint8_t r = 0; r < 5; ++r) {
    uint8_t record[A4_UC8279C_LUT_LENGTH] = {};
    memcpy(record, A4_UC8279C_GRAY3_TRANSITION_LUT[r], A4_UC8279C_GRAY3_RECORD_LENGTH);
    if (r == 4 && !anyWhiteClear) record[2] = A4_UC8279C_GRAY3_NO_WHITE_CLEAR;
    if (r == 2 && !anyBlack) record[1] = A4_UC8279C_GRAY3_NO_BLACK_DRIVE;
    if (r == 3 && !anyBlack) record[2] = A4_UC8279C_GRAY3_NO_BLACK_GRAY;
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + r), record, A4_UC8279C_LUT_LENGTH);
  }
}

void Uc8279cA4Driver::showGray3(EpdBus& bus) {
  if (!_gray3Session) {
    hardwareReset(bus);
    initController(bus, Session::Gray3);
  } else {
    ensurePowerOn(bus);
  }
  bool anyBlack = false;
  bool anyWhiteClear = false;
  writeGray3Codes(bus, true, anyBlack, anyWhiteClear);
  writeGray3Codes(bus, false, anyBlack, anyWhiteClear);
  const uint8_t interval = 0x00;
  bus.cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);
  loadGray3Lut(bus, anyBlack, anyWhiteClear);
  refresh(bus);
  const size_t n = static_cast<size_t>(_widthBytes) * _height;
  memcpy(_gray3White, _grayLsb, n);
  memcpy(_gray3Middle, _grayMsb, n);
  _gray3HistoryValid = true;
  _gray3Session = true;
  _firstRefreshPending = false;
  _redriveAfterGray = true;
}

void Uc8279cA4Driver::displayGray(EpdBus& bus, const uint8_t* fb, const bool turnOff, const unsigned char* lut,
                                  const bool factoryMode) {
  (void)fb;
  (void)lut;
  (void)factoryMode;
  // Allocation failure means the already-painted BW page remains visible;
  // never substitute the last rendered bit-plane as a black/white frame.
  const bool overlay = _overlayPass;
  _overlayPass = true;  // overlay unless the next pass is begun as absolute
  if (overlay) {
    if (!_grayLsb || !_grayMsb || !_grayBase || !allocateGray3()) {
      if (_basePending) display(bus, _grayBase, nullptr, _cleanupPage ? RefreshMode::Half : RefreshMode::Fast, turnOff);
      return;
    }
    const size_t n = static_cast<size_t>(_widthBytes) * _height;
    if (!_gray3HistoryValid) {
      // Coming from a B/W screen: show the base with a normal B/W refresh,
      // which fully drives out the previous screen, and start the Gray3
      // history from it. Stock's history-less Gray3 drive clears too weakly
      // and leaves the previous screen ghosting.
      if (_basePending) display(bus, _grayBase, nullptr, _cleanupPage ? RefreshMode::Half : RefreshMode::Fast, false);
      _cleanupPage = false;
      memcpy(_gray3White, _grayBase, n);
      memset(_gray3Middle, 0, n);
      _gray3HistoryValid = true;
    }
    // Masks (LSB = dark, MSB = dark | light) onto the base: any gray pixel is
    // middle, the rest keep the base's white/black.
    for (size_t i = 0; i < n; ++i) {
      const uint8_t gray = _grayMsb[i];
      _grayLsb[i] = static_cast<uint8_t>(_grayBase[i] & ~gray);
      _grayMsb[i] = gray;
    }
    if (_cleanupPage) {
      // Stock's periodic cleanup: the page with black and white swapped
      // (middle kept), then the page itself, both as Gray3 transitions.
      for (size_t i = 0; i < n; ++i) _grayLsb[i] = static_cast<uint8_t>(~(_grayLsb[i] | _grayMsb[i]));
      showGray3(bus);
      for (size_t i = 0; i < n; ++i) _grayLsb[i] = static_cast<uint8_t>(~(_grayLsb[i] | _grayMsb[i]));
    }
    showGray3(bus);
    _basePending = false;
    _cleanupPage = false;
    return;
  }
  if (!_grayLsb || !_grayMsb) return;
  _gray3HistoryValid = false;
  _gray3Session = false;

  hardwareReset(bus);
  initController(bus, Session::FourGray);
  // Stock wire order: MSB -> DTM1 (0x10), LSB -> DTM2 (0x13), uninverted, so
  // black/dark/light/white = 00/01/10/11 in (DTM1, DTM2).
  writeFrame(bus, CMD_DTM1, _grayMsb);
  writeFrame(bus, CMD_DTM2, _grayLsb);
  // Both gray planes are fully written, replacing the unknown startup state.
  _firstRefreshPending = false;
  loadGrayLut(bus);
  refresh(bus);
  _redriveAfterGray = true;
}

void Uc8279cA4Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  (void)bw;
  // A stashed base whose gray pass never ran (out of memory, canceled) still has
  // to reach the panel. After a gray pass there is nothing to restore: the next
  // B/W refresh re-initializes the controller and re-drives every pixel.
  if (_basePending && _grayBase) display(bus, _grayBase, nullptr, _cleanupPage ? RefreshMode::Half : RefreshMode::Fast, false);
  _basePending = false;
}

void Uc8279cA4Driver::deepSleep(EpdBus& bus) {
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
