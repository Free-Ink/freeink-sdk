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
  // Refreshes leave the rails up: discharge them before reset.
  if (_screenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" A4 power-off");
    _screenOn = false;
  }
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
  // Every session uses stock V1.0.3's power and VCOM_DC settings. With the
  // earlier B/W rails (PWR 43 00 78 78 17), static screens fuzzed while idle on
  // hardware at every B/W VCOM_DC tried (0x24, 0x26, 0x32), on different units.
  const uint8_t panel[] = {0x3f, 0x4a};
  const uint8_t power[] = {0x03, 0x00, 0x72, 0x72};
  const uint8_t booster[] = {0x25, 0x25, 0x3c};
  const uint8_t resolution[] = {0x03, 0x00, 0x02, 0x58};  // 768x600
  const uint8_t interval[] = {0x00, 0x00, 0x00, 0x00};
  const uint8_t powerOffSequence = 0x20;
  const uint8_t vcom = 0x32;
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

// Stock's lockscreen pre-clear: the GC waveform with every phase's frame count
// halved, (b & 0xc0) | (((b & 0x3f) + 1) >> 1) on bytes 1-4 of each 7-byte group.
void Uc8279cA4Driver::loadHalfFullLut(EpdBus& bus) {
  const uint8_t* const lut[] = {A4_UC8279C_LUT_20, A4_UC8279C_LUT_21, A4_UC8279C_LUT_22, A4_UC8279C_LUT_23,
                                A4_UC8279C_LUT_24};
  for (uint8_t i = 0; i < 5; ++i) {
    uint8_t record[A4_UC8279C_LUT_LENGTH];
    memcpy(record, lut[i], sizeof(record));
    for (uint8_t g = 0; g + 7 <= sizeof(record); g += 7) {
      for (uint8_t k = 1; k <= 4; ++k) {
        const uint8_t b = record[g + k];
        record[g + k] = static_cast<uint8_t>((b & 0xc0) | (((b & 0x3f) + 1) >> 1));
      }
    }
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), record, sizeof(record));
  }
}

void Uc8279cA4Driver::loadFastLut(EpdBus& bus, const bool longToBlack) {
  const uint8_t* const lut[] = {A4_UC8279C_FAST_LUT_20, A4_UC8279C_FAST_LUT_21, A4_UC8279C_FAST_LUT_22,
                                A4_UC8279C_FAST_LUT_23, A4_UC8279C_FAST_LUT_24};
  for (uint8_t i = 0; i < 5; ++i) {
    uint8_t record[A4_UC8279C_LUT_LENGTH];
    memcpy(record, lut[i], sizeof(record));
    // The to-black record (0x23) and the VCOM record gain the extra frames.
    if (longToBlack && (i == 0 || i == 3)) record[1] = static_cast<uint8_t>(record[1] + A4_UC8279C_FAST_AFTER_UI_EXTRA_FRAMES);
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), record, sizeof(record));
  }
}

// Stock Factory B drives white pixels only about 12 frames toward white after
// the black pre-clear; on hardware, light and white areas then showed charge
// left by earlier screens. Record 0x21 (white) gets its last two white pulses
// lengthened to 6 frames each (stock 0x81, 0x82).
void Uc8279cA4Driver::loadGrayLut(EpdBus& bus) {
  for (uint8_t i = 0; i < 5; ++i) {
    uint8_t record[A4_UC8279C_GRAY_RECORD_LENGTH];
    memcpy(record, A4_UC8279C_GRAY_LUT + i * A4_UC8279C_GRAY_RECORD_LENGTH, sizeof(record));
    if (i == 1) {
      record[15] = 0x86;
      record[17] = 0x86;
    }
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), record, sizeof(record));
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

void Uc8279cA4Driver::refresh(EpdBus& bus, const uint8_t* bwBaseline, const bool runIdleHook) {
  ensurePowerOn(bus);
  bus.cmd(CMD_DISPLAY_REFRESH);
  bool refreshDone = false;
  if (runIdleHook && _idleHook) {
    const auto hook = _idleHook;
    const auto ctx = _idleHookCtx;
    clearIdleHook();
    // BUSY takes a moment to assert. Run the hook only once the waveform is
    // confirmed running, so a refresh that ends during it is not mistaken for
    // one that never started.
    const unsigned long start = millis();
    bool running = bus.isBusy();
    while (!running && millis() - start < 100) {
      delay(1);
      running = bus.isBusy();
    }
    if (running) {
      hook(ctx);
      refreshDone = !bus.isBusy();
    }
  }
  if (!refreshDone) bus.waitBusy(" A4 refresh");
  _lastRefreshMs = millis();
  // The next differential update diffs against this baseline.
  if (bwBaseline) writeFrame(bus, CMD_DTM1, bwBaseline);
  // Stock V1.0.3 boots with panel power hold on and leaves the rails up after
  // every refresh; only its lockscreen paint and deep sleep power off. Here
  // hardwareReset(), the four-gray paint and deepSleep() power a held panel off.
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
  (void)turnOff;  // the panel stays powered after refresh(), as on stock
  _overlayPass = true;  // a B/W paint ends an announced absolute pass
  displayBw(bus, fb, mode);
  // A B/W refresh never runs the idle hook; it must not outlive this call.
  clearIdleHook();
}

void Uc8279cA4Driver::displayBw(EpdBus& bus, const uint8_t* fb, const RefreshMode mode) {
  // The Gray3 history, while valid, is what the panel shows.
  const bool afterGray3 = _gray3HistoryValid && _gray3Code;
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
  // The first book page after a menu or other screen: its fast paint drives
  // pixels turning black for longer, with no flash.
  const bool pageAfterUi = _readerRefresh && _uiShown;
  _uiShown = !_readerRefresh;
  const bool fast = mode == RefreshMode::Fast && !_firstRefreshPending;
  const uint8_t interval = fast ? 0xd7 : 0x97;
  bus.cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);
  if (fast) {
    // Stock's production fast (DU) waveform.
    loadFastLut(bus, pageAfterUi);
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
  if (!fast && !_firstRefreshPending) {
    // In the full bank, 0x21 (white to white) and 0x24 (black to black) flash
    // the pixel through both states; 0x22 and 0x23 drive a changing pixel one
    // way only. OLD = NEW puts every pixel on a flashing record, so pixels that
    // changed and pixels that held get the same drive.
    writeFrame(bus, CMD_DTM1, fb);
  } else if (afterGray3 && !_firstRefreshPending) {
    // Fast refresh straight after a Gray3 page: OLD is the page as the panel
    // shows it, with each gray pixel as the inverse of its target so that it
    // is driven. White that stays white is not driven.
    const size_t n = static_cast<size_t>(_widthBytes) * _height;
    uint8_t* old = _gray3Code;
    for (size_t i = 0; i < n; ++i) old[i] = static_cast<uint8_t>(_gray3White[i] | (_gray3Middle[i] & ~fb[i]));
    writeFrame(bus, CMD_DTM1, old);
  } else if (_firstRefreshPending || _redriveAfterGray) {
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
  // PSRAM only: a panel-sized buffer that does not fit there would not fit in
  // the internal heap either, and should not be taken from it.
  return static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void Uc8279cA4Driver::beginGrayscale(EpdBus& bus, const uint8_t* fb, const GrayscaleMode mode,
                                     const RefreshMode fallback, const bool turnOff) {
  _overlayPass = mode == GrayscaleMode::Overlay;
  if (!_overlayPass) return;  // absolute planes carry every pixel: no base pass
  // Gray3 sends the base with its grays in one refresh: stash it undisplayed.
  if (!fb) return;
  if (!_grayBase) _grayBase = allocateGrayBuffer();
  if (!_grayBase) {
    // No stash buffer: show the B/W frame now so the page still reaches the panel.
    display(bus, fb, nullptr, fallback, turnOff);
    return;
  }
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
  if (target)
    memcpy(target + static_cast<size_t>(yStart) * _widthBytes, rows, static_cast<size_t>(numRows) * _widthBytes);
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
  if (!_gray3Code) _gray3Code = allocateGrayBuffer();
  return _gray3White && _gray3Middle && _gray3Code;
}

// Gray3 page: _grayBase (white / black) with the gray mask (_grayMsb) on top.
// Each pixel's code comes from its new level and the level already on the
// panel (stock V1.0.3's codes): unchanged white and gray pixels hold, black is
// always re-driven, a pixel turning white is cleared, a new gray pixel takes
// the gray record. The codes are worked out a row at a time on the stack: the
// DTM1 plane goes straight to the bus and the DTM2 plane waits in _gray3Code.
// The same pass records the page as the new Gray3 history.
void Uc8279cA4Driver::writeGray3Page(EpdBus& bus, const bool inverted, Gray3Use& use) {
  constexpr uint16_t MAX_ROW = 96;
  const uint16_t w = _widthBytes < MAX_ROW ? _widthBytes : MAX_ROW;
  uint8_t base[MAX_ROW], gray[MAX_ROW], wasWhite[MAX_ROW], wasGray[MAX_ROW];
  uint8_t hi[MAX_ROW], lo[MAX_ROW];
  uint8_t anyBlack = 0, anyWhiteClear = 0;
  bus.cmd(CMD_DTM1);
  auto txn = bus.beginTxn();
  size_t out = 0;
  for (int y = static_cast<int>(_height) - 1; y >= 0; --y, out += _widthBytes) {
    const size_t row = static_cast<size_t>(y) * _widthBytes;
    memcpy(base, _grayBase + row, w);
    memcpy(gray, _grayMsb + row, w);
    memcpy(wasWhite, _gray3White + row, w);
    memcpy(wasGray, _gray3Middle + row, w);
    for (uint16_t xb = 0; xb < w; ++xb) {
      const uint8_t g = gray[xb];
      const auto white = static_cast<uint8_t>(inverted ? ~(base[xb] | g) : base[xb] & ~g);
      const auto black = static_cast<uint8_t>(~(white | g));
      const auto newGray = static_cast<uint8_t>(g & ~wasGray[xb]);
      const auto whiteClear = static_cast<uint8_t>(white & ~wasWhite[xb]);
      // 10 black, 01 gray, 11 white-clear, 00 hold. DTM1 = bit 1.
      hi[xb] = static_cast<uint8_t>(whiteClear | black);
      lo[xb] = static_cast<uint8_t>(whiteClear | newGray);
      anyBlack |= static_cast<uint8_t>(hi[xb] & ~lo[xb]);
      anyWhiteClear |= static_cast<uint8_t>(hi[xb] & lo[xb]);
      wasWhite[xb] = white;
    }
    txn.writeBytes(hi, w);
    memcpy(_gray3Code + out, lo, w);
    memcpy(_gray3White + row, wasWhite, w);
    memcpy(_gray3Middle + row, gray, w);
  }
  memset(hi, 0x00, sizeof(hi));  // padding rows hold
  for (uint16_t y = 0; y < PADDING_ROWS; ++y) txn.writeBytes(hi, w);
  use.black = anyBlack != 0;
  use.whiteClear = anyWhiteClear != 0;
}

void Uc8279cA4Driver::sendGray3Plane(EpdBus& bus, const bool dtm1, const uint8_t* codes) {
  constexpr size_t CHUNK = 4096;
  const size_t n = static_cast<size_t>(_widthBytes) * _height;
  bus.cmd(dtm1 ? CMD_DTM1 : CMD_DTM2);
  auto txn = bus.beginTxn();
  for (size_t at = 0; at < n; at += CHUNK)
    txn.writeBytes(codes + at, static_cast<uint16_t>(n - at < CHUNK ? n - at : CHUNK));
  uint8_t hold[96] = {};  // padding rows hold
  for (uint16_t y = 0; y < PADDING_ROWS; ++y) txn.writeBytes(hold, _widthBytes);
}

// Stock's black and white-clear records with its per-page patches. The gray
// record (0x23) is not stock's: it is the black record's drive followed, in
// the second group, by white for the gray frames (the hold phase after it
// keeps the group's length), so a gray pixel is drawn black with the ink and
// lightened in the same refresh.
void Uc8279cA4Driver::loadGray3Lut(EpdBus& bus, const bool anyBlack, const bool anyWhiteClear) {
  for (uint8_t r = 0; r < 5; ++r) {
    uint8_t record[A4_UC8279C_LUT_LENGTH] = {};
    memcpy(record, A4_UC8279C_GRAY3_TRANSITION_LUT[r == 3 ? 2 : r], A4_UC8279C_GRAY3_RECORD_LENGTH);
    if (r == 4 && !anyWhiteClear) record[2] = A4_UC8279C_GRAY3_NO_WHITE_CLEAR;
    if (r == 2 && !anyBlack) record[1] = A4_UC8279C_GRAY3_NO_BLACK_DRIVE;
    if (r == 3) {
      const uint8_t held = static_cast<uint8_t>(record[8] + record[9]);
      record[8] = static_cast<uint8_t>(0x80 | A4_UC8279C_GRAY3_GRAY_WHITE_FRAMES);
      record[9] = static_cast<uint8_t>(held - A4_UC8279C_GRAY3_GRAY_WHITE_FRAMES);
    }
    bus.cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + r), record, A4_UC8279C_LUT_LENGTH);
  }
}

void Uc8279cA4Driver::showGray3(EpdBus& bus, const bool inverted) {
  if (!_gray3Session) {
    hardwareReset(bus);
    initController(bus, Session::Gray3);
  } else {
    ensurePowerOn(bus);
  }
  Gray3Use use;
  writeGray3Page(bus, inverted, use);
  sendGray3Plane(bus, false, _gray3Code);
  const uint8_t interval = 0x00;
  bus.cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);
  loadGray3Lut(bus, use.black, use.whiteClear);
  refresh(bus, nullptr, /*runIdleHook=*/true);
  _gray3HistoryValid = true;
  _gray3Session = true;
  _firstRefreshPending = false;
  _redriveAfterGray = true;
  _uiShown = false;
}

void Uc8279cA4Driver::displayGray(EpdBus& bus, const uint8_t* fb, const bool turnOff, const unsigned char* lut,
                                  const bool factoryMode) {
  (void)fb;
  (void)lut;
  (void)factoryMode;
  // The idle hook lasts for this call only: whichever way it returns (out of
  // memory, four-gray planes, or a Gray3 refresh that did not run it), a hook
  // left set would fire on some later, unrelated page turn.
  struct IdleHookScope {
    Uc8279cA4Driver& driver;
    ~IdleHookScope() { driver.clearIdleHook(); }
  } idleHookScope{*this};
  // Allocation failure means the already-painted BW page remains visible;
  // never substitute the last rendered bit-plane as a black/white frame.
  const bool overlay = _overlayPass;
  _overlayPass = true;  // overlay unless the next pass is begun as absolute
  if (overlay) {
    if (!_grayLsb || !_grayMsb || !_grayBase || !allocateGray3()) {
      if (_basePending) displayBw(bus, _grayBase, _cleanupPage ? RefreshMode::Half : RefreshMode::Fast);
      return;
    }
    const size_t n = static_cast<size_t>(_widthBytes) * _height;
    // One gray level: the dark level (both masks) is drawn black. Those
    // pixels leave the gray mask and stay black in the base.
    for (size_t i = 0; i < n; ++i) _grayMsb[i] &= static_cast<uint8_t>(~_grayLsb[i]);
    if (!_gray3HistoryValid) {
      // Coming from a B/W screen: show the base with a normal B/W refresh,
      // which fully drives out the previous screen, and start the Gray3
      // history from it. Stock's history-less Gray3 drive clears too weakly
      // and leaves the previous screen ghosting.
      // displayBw(), not display(): the hook is still wanted for the Gray3 refresh below.
      if (_basePending) displayBw(bus, _grayBase, RefreshMode::Half);
      _cleanupPage = false;
      memcpy(_gray3White, _grayBase, n);
      memset(_gray3Middle, 0, n);
      _gray3HistoryValid = true;
    }
    // Stock's periodic cleanup: the page with black and white swapped (grays
    // kept), then the page itself, both as Gray3 transitions.
    if (_cleanupPage) showGray3(bus, /*inverted=*/true);
    showGray3(bus, /*inverted=*/false);
    _basePending = false;
    _cleanupPage = false;
    return;
  }
  if (!_grayLsb || !_grayMsb) return;
  _gray3HistoryValid = false;
  _gray3Session = false;

  // Stock V1.0.3's lockscreen sequence. Pre-clear ("GC-black-2x-half") in the
  // B/W session: black to DTM2, no DTM1 write, two refreshes with the halved
  // GC waveform.
  if (_grayControllerMode) {
    hardwareReset(bus);
    initController(bus, Session::Bw);
  }
  ensurePowerOn(bus);
  const uint8_t clearInterval = 0x97;
  bus.cmdData(CMD_VCOM_DATA_INTERVAL, &clearInterval, 1);
  loadHalfFullLut(bus);
  fillControllerRam(bus, CMD_DTM2, 0x00);
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" A4 refresh");
  refresh(bus);

  hardwareReset(bus);
  initController(bus, Session::FourGray);
  // Stock wire order: MSB -> DTM1 (0x10), LSB -> DTM2 (0x13), uninverted, so
  // black/dark/light/white = 00/01/10/11 in (DTM1, DTM2).
  writeFrame(bus, CMD_DTM1, _grayMsb);
  writeFrame(bus, CMD_DTM2, _grayLsb);
  // Both gray planes are fully written, replacing the unknown startup state.
  _firstRefreshPending = false;
  loadGrayLut(bus);
  ensurePowerOn(bus);
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" A4 refresh");
  // Stock re-seeds the old plane white before powering off.
  fillControllerRam(bus, CMD_DTM1, 0xff);
  bus.cmd(CMD_POWER_OFF);
  bus.waitBusy(" A4 power-off");
  _screenOn = false;
  _redriveAfterGray = true;
}

void Uc8279cA4Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  (void)bw;
  // A stashed base whose gray pass never ran (out of memory, canceled) still has
  // to reach the panel. After a gray pass there is nothing to restore: the next
  // B/W refresh re-initializes the controller and re-drives every pixel.
  if (_basePending && _grayBase)
    display(bus, _grayBase, nullptr, _cleanupPage ? RefreshMode::Half : RefreshMode::Fast, false);
  _basePending = false;
}

// Stock V1.0.3's idle power-off: 500 ms after a refresh, with no further work
// pending, it sends POWER_OFF (logged "EPD reader idle POF") and the next
// refresh powers the panel back on. The controller keeps its registers and RAM.
// The delay is timed here, from the last refresh, so callers can call this
// whenever they are otherwise idle.
void Uc8279cA4Driver::controllerIdle(EpdBus& bus) {
  constexpr unsigned long IDLE_POWER_OFF_MS = 500;
  if (!_screenOn || _basePending || millis() - _lastRefreshMs < IDLE_POWER_OFF_MS) return;
  bus.cmd(CMD_POWER_OFF);
  bus.waitBusy(" A4 power-off");
  _screenOn = false;
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
