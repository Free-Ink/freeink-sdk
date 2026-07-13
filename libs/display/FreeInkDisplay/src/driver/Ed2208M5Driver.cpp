#include "Ed2208M5Driver.h"

#include <BoardConfig.h>
#include <M5Pm1.h>

#include <algorithm>
#include <cstring>

// Native M5Stack PaperColor (ED2208) driver, ported from the upstream
// community-sdk feat-m5-paper-color branch. See Ed2208M5Driver.h for why the
// refresh is interrupted at ~340 ms (6-color panel, white settles last).

namespace freeink {
namespace {

// Fast (interrupted) refresh appearance. Default 0 = light "paper" UI: the
// straight pixel mapping sends logical white as the controller's white code,
// whose waveform — cut off at ~340 ms, before white settles — lands yellow.
// Set -DFREEINK_M5_DARK_FAST_REFRESH=1 for the upstream community-sdk dark
// hack instead: logical white written as controller black, giving an inverted
// black-background UI on fast refreshes (complete waveforms stay truthful).
#ifndef FREEINK_M5_DARK_FAST_REFRESH
#define FREEINK_M5_DARK_FAST_REFRESH 0
#endif

// Physical panel geometry (the app-facing framebuffer is 600x400 landscape).
constexpr uint16_t PANEL_WIDTH = 400;
constexpr uint16_t PANEL_HEIGHT = 600;
constexpr uint16_t REFRESH_CUTOFF_MS = 340;
constexpr uint16_t BUSY_SETTLE_MS = 20;
constexpr uint8_t DARK_DISPLAY_CTRL = 0x0F;
constexpr uint32_t PANEL_AREA = static_cast<uint32_t>(PANEL_WIDTH) * PANEL_HEIGHT;

}  // namespace

uint32_t Ed2208M5Driver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 4000000;
}

PanelGeometry Ed2208M5Driver::geometry() const { return {LOGICAL_W, LOGICAL_H, LOGICAL_WB, LOGICAL_BUF}; }
int8_t Ed2208M5Driver::spiMiso() const { return BoardConfig::ACTIVE.sd.miso; }
int8_t Ed2208M5Driver::coCs() const { return BoardConfig::ACTIVE.sd.cs; }

void Ed2208M5Driver::enablePmicPower() {
  // The display is the first M5PM1 caller at boot; it establishes the board power
  // policy (charge+boost on, RGB rail off — see m5pm1::applyBootPowerPolicy) and
  // then routes the EPD rail. LedManager shares the same single PM1 owner.
  m5pm1::beginBus();
  m5pm1::applyBootPowerPolicy();
  // EPD_EN is PMIC GPIO0: plain push-pull output, driven high.
  m5pm1::updateReg(m5pm1::REG_GPIO_FUNC0, m5pm1::GPIO0, 0);
  m5pm1::updateReg(m5pm1::REG_GPIO_MODE, 0, m5pm1::GPIO0);
  m5pm1::updateReg(m5pm1::REG_GPIO_DRV, m5pm1::GPIO0, 0);
  m5pm1::updateReg(m5pm1::REG_GPIO_OUT, 0, m5pm1::GPIO0);
  delay(100);
}

void Ed2208M5Driver::waitBusy(EpdBus& bus) {
  const int8_t busyPin = bus.pins().busy;
  const unsigned long start = millis();
  bool busy = digitalRead(busyPin) == LOW;
  if (!busy) {
    while (millis() - start < 100) {
      if (digitalRead(busyPin) == LOW) {
        busy = true;
        break;
      }
      delay(1);
    }
  }
  if (busy) {
    do {
      delay(10);
      if (millis() - start > 30000) break;
    } while (digitalRead(busyPin) == LOW);
    delay(BUSY_SETTLE_MS);
  }
}

void Ed2208M5Driver::initController(EpdBus& bus) {
  static constexpr uint8_t initCommands[] = {
      0xAA, 6, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18,  // CMDH
      0x01, 1, 0x3F,
      0x00, 2, 0x5F, 0x69,
      0x05, 4, 0x40, 0x1F, 0x1F, 0x2C,
      0x08, 4, 0x6F, 0x1F, 0x1F, 0x22,
      0x06, 4, 0x6F, 0x1F, 0x17, 0x17,
      0x03, 4, 0x03, 0x54, 0x00, 0x44,
      0x60, 2, 0x02, 0x00,
      0x30, 1, 0x08,
      0x50, 1, DARK_DISPLAY_CTRL,
      0xE3, 1, 0x2F,
      0x84, 1, 0x01,
  };

  auto txn = bus.beginTxn();
  for (size_t i = 0; i < sizeof(initCommands);) {
    const uint8_t command = initCommands[i++];
    const uint8_t length = initCommands[i++];
    waitBusy(bus);
    txn.cmd(command);
    for (uint8_t j = 0; j < length; ++j) {
      txn.data(initCommands[i++]);
    }
  }
  waitBusy(bus);
  txn.cmd(0x61);
  txn.data(static_cast<uint8_t>((PANEL_WIDTH >> 8) & 0xFF));
  txn.data(static_cast<uint8_t>(PANEL_WIDTH & 0xFF));
  txn.data(static_cast<uint8_t>((PANEL_HEIGHT >> 8) & 0xFF));
  txn.data(static_cast<uint8_t>(PANEL_HEIGHT & 0xFF));
  txn.end();
}

void Ed2208M5Driver::begin(EpdBus& bus) {
  enablePmicPower();
  bus.reset();
  initController(bus);
  memset(_prevFrame, 0xFF, LOGICAL_BUF);
  _panelPowerOn = false;
  _lastFrameValid = false;
  _completeNextRefresh = false;
}

void Ed2208M5Driver::writeFrame(EpdBus& bus, const uint8_t* fb) {
  // Pixel codes 0x0/0x1 are the controller's black/white. The straight mapping
  // is correct for both paths in light mode: a complete waveform settles
  // truthfully (true white), and an interrupted one leaves logical-white
  // pixels yellow (the white track cut early) — a light paper-style UI.
  // In dark mode only the fast path swaps (the upstream dark hack); complete
  // waveforms must stay straight or they display inverted. writeFrame runs
  // before refresh() consumes the one-shot flag, so it can pick per-frame.
#if FREEINK_M5_DARK_FAST_REFRESH
  const uint8_t EPD_WHITE = _completeNextRefresh ? 0x1 : 0x0;
  const uint8_t EPD_BLACK = _completeNextRefresh ? 0x0 : 0x1;
#else
  constexpr uint8_t EPD_WHITE = 0x1;
  constexpr uint8_t EPD_BLACK = 0x0;
#endif
  uint8_t packedRow[PANEL_WIDTH / 2];

  auto txn = bus.beginTxn();
  txn.cmd(0x10);
  for (uint16_t panelY = 0; panelY < PANEL_HEIGHT; ++panelY) {
    for (uint16_t panelX = 0; panelX < PANEL_WIDTH; panelX += 2) {
      const uint16_t leftLogicalX = panelY;
      const uint16_t leftLogicalY = static_cast<uint16_t>(LOGICAL_H - 1 - panelX);
      const uint32_t leftOffset = static_cast<uint32_t>(leftLogicalY) * LOGICAL_WB;
      const bool leftWhite = (fb[leftOffset + (leftLogicalX >> 3)] >> (7 - (leftLogicalX & 7))) & 0x01;

      const uint16_t rightPanelX = static_cast<uint16_t>(panelX + 1);
      const uint16_t rightLogicalX = panelY;
      const uint16_t rightLogicalY = static_cast<uint16_t>(LOGICAL_H - 1 - rightPanelX);
      const uint32_t rightOffset = static_cast<uint32_t>(rightLogicalY) * LOGICAL_WB;
      const bool rightWhite = (fb[rightOffset + (rightLogicalX >> 3)] >> (7 - (rightLogicalX & 7))) & 0x01;

      packedRow[panelX >> 1] = static_cast<uint8_t>(((leftWhite ? EPD_WHITE : EPD_BLACK) << 4) |
                                                    (rightWhite ? EPD_WHITE : EPD_BLACK));
    }
    txn.writeBytes(packedRow, sizeof(packedRow));
  }
  txn.end();
}

void Ed2208M5Driver::setPartialWindow(EpdBus::Transaction& txn, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  if (w == 0 || h == 0) return;
  const uint16_t xEnd = static_cast<uint16_t>(x + w - 1);
  const uint16_t yEnd = static_cast<uint16_t>(y + h);
  const uint8_t window[9] = {
      static_cast<uint8_t>(x >> 8),    static_cast<uint8_t>(x & 0xFF),
      static_cast<uint8_t>(xEnd >> 8), static_cast<uint8_t>(xEnd & 0xFF),
      static_cast<uint8_t>(y >> 8),    static_cast<uint8_t>(y & 0xFF),
      static_cast<uint8_t>(yEnd >> 8), static_cast<uint8_t>(yEnd & 0xFF),
      0x01,
  };
  txn.cmd(0x83);
  for (uint8_t v : window) txn.data(v);
}

void Ed2208M5Driver::powerOn(EpdBus& bus) {
  if (_panelPowerOn) return;
  auto txn = bus.beginTxn();
  txn.cmd(0x04);
  waitBusy(bus);
  txn.end();
  _panelPowerOn = true;
}

void Ed2208M5Driver::powerOff(EpdBus& bus) {
  if (!_panelPowerOn) return;
  auto txn = bus.beginTxn();
  txn.cmd(0x02);
  txn.data(0x00);
  waitBusy(bus);
  txn.end();
  _panelPowerOn = false;
}

void Ed2208M5Driver::setFastRefreshCutoffMs(uint16_t ms) {
  _cutoffMs = ms < 100 ? 100 : (ms > 2000 ? 2000 : ms);
}

uint16_t Ed2208M5Driver::fastRefreshCutoffMs() const {
  return _cutoffMs ? _cutoffMs : REFRESH_CUTOFF_MS;
}

void Ed2208M5Driver::interruptRefresh(EpdBus& bus) {
  // Abort the OTP waveform before white settles. CRITICAL: anchor the cutoff
  // to BUSY's falling edge (the drive actually starting), NOT the 0x12
  // command. The controller preps before driving — power ramp, on-die
  // temperature read (which calibrates the waveform), gate setup — and that
  // prep time varies per refresh. A command-anchored delay therefore cuts at
  // a different waveform phase every time: mostly mid dark/blue phases (the
  // muddy, blue-banded look), occasionally just after the white push (the
  // "randomly looks fantastic" refresh). Edge-anchoring makes the cut phase
  // — and therefore the panel's appearance — deterministic and tunable via
  // setFastRefreshCutoffMs().
  const int8_t busyPin = bus.pins().busy;
  const unsigned long t0 = millis();
  while (digitalRead(busyPin) == HIGH && millis() - t0 < 500) {
    delayMicroseconds(200);
  }
  delay(_cutoffMs ? _cutoffMs : REFRESH_CUTOFF_MS);
  bus.reset();
  initController(bus);
  _panelPowerOn = false;
}

bool Ed2208M5Driver::getDirtyWindow(const uint8_t* fb, uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h) const {
  if (!_lastFrameValid) {
    *x = 0;
    *y = 0;
    *w = PANEL_WIDTH;
    *h = PANEL_HEIGHT;
    return true;
  }

  uint16_t minPanelX = PANEL_WIDTH, minPanelY = PANEL_HEIGHT, maxPanelX = 0, maxPanelY = 0;
  bool any = false;
  for (uint16_t logicalY = 0; logicalY < LOGICAL_H; ++logicalY) {
    const uint32_t rowOffset = static_cast<uint32_t>(logicalY) * LOGICAL_WB;
    for (uint16_t byteX = 0; byteX < LOGICAL_WB; ++byteX) {
      if (fb[rowOffset + byteX] == _prevFrame[rowOffset + byteX]) continue;
      for (uint8_t bit = 0; bit < 8; ++bit) {
        const uint16_t logicalX = static_cast<uint16_t>(byteX * 8 + bit);
        if (logicalX >= LOGICAL_W) break;
        const uint8_t mask = static_cast<uint8_t>(0x80 >> bit);
        if ((fb[rowOffset + byteX] & mask) == (_prevFrame[rowOffset + byteX] & mask)) continue;
        const uint16_t panelX = static_cast<uint16_t>(LOGICAL_H - 1 - logicalY);
        const uint16_t panelY = logicalX;
        minPanelX = std::min(minPanelX, panelX);
        maxPanelX = std::max(maxPanelX, panelX);
        minPanelY = std::min(minPanelY, panelY);
        maxPanelY = std::max(maxPanelY, panelY);
        any = true;
      }
    }
  }
  if (!any) return false;

  constexpr uint16_t PAD = 8;
  minPanelX = static_cast<uint16_t>((minPanelX > PAD) ? minPanelX - PAD : 0);
  minPanelY = static_cast<uint16_t>((minPanelY > PAD) ? minPanelY - PAD : 0);
  maxPanelX = std::min<uint16_t>(PANEL_WIDTH - 1, maxPanelX + PAD);
  maxPanelY = std::min<uint16_t>(PANEL_HEIGHT - 1, maxPanelY + PAD);
  *x = minPanelX;
  *y = minPanelY;
  *w = static_cast<uint16_t>(maxPanelX - minPanelX + 1);
  *h = static_cast<uint16_t>(maxPanelY - minPanelY + 1);
  return true;
}

void Ed2208M5Driver::refresh(EpdBus& bus, uint16_t dirtyX, uint16_t dirtyY, uint16_t dirtyW, uint16_t dirtyH) {
  const bool completeWaveform = _completeNextRefresh;
  _completeNextRefresh = false;
  powerOn(bus);

  auto txn = bus.beginTxn();
  txn.cmd(0x06);
  txn.data(0x6F);
  txn.data(0x1F);
  txn.data(0x17);
  txn.data(0x27);
  setPartialWindow(txn, dirtyX, dirtyY, dirtyW, dirtyH);
  txn.cmd(0x50);
  // Complete refresh uses the vendor VCOM/CDI for full contrast; the fast
  // interrupt path keeps the dark-hack value.
  txn.data(completeWaveform ? 0x3F : DARK_DISPLAY_CTRL);
  txn.cmd(0x12);
  txn.data(0x00);
  txn.end();

  if (completeWaveform) {
    // Run the full OTP waveform to completion. waitBusy()'s generic 100 ms
    // assert window is too short here: the controller preps (power ramp,
    // on-die temperature read) for a variable time before BUSY drops, and
    // when prep exceeds the window waitBusy() concludes the panel is idle —
    // so the POWER_OFF below lands mid-waveform, turning the "complete"
    // refresh into an accidental interrupted one (nondeterministic blue
    // gate-scan band, the very artifact this path exists to avoid). Wait
    // generously for the drive to actually start, then for it to finish.
    const int8_t busyPin = bus.pins().busy;
    const unsigned long start = millis();
    while (digitalRead(busyPin) == HIGH && millis() - start < 2000) {
      delay(1);
    }
    while (digitalRead(busyPin) == LOW && millis() - start < 60000) {
      delay(10);
    }
    delay(BUSY_SETTLE_MS);
    auto powerOffTxn = bus.beginTxn();
    powerOffTxn.cmd(0x02);  // POWER_OFF
    powerOffTxn.data(0x00);
    waitBusy(bus);
    powerOffTxn.end();
    _panelPowerOn = false;
  } else {
    interruptRefresh(bus);
  }
}

void Ed2208M5Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  if (!fb) return;

  uint16_t dirtyX = 0, dirtyY = 0, dirtyW = PANEL_WIDTH, dirtyH = PANEL_HEIGHT;
  const bool forceFullPanelRefresh = (mode == RefreshMode::Full);

  if (!forceFullPanelRefresh && !getDirtyWindow(fb, &dirtyX, &dirtyY, &dirtyW, &dirtyH)) {
    if (turnOff) deepSleep(bus);
    return;
  }
  // Dual-buffer early-out: identical frame, nothing to do.
  if (!forceFullPanelRefresh && _lastFrameValid && prev != nullptr && memcmp(prev, fb, LOGICAL_BUF) == 0) {
    if (turnOff) deepSleep(bus);
    return;
  }

  // ED2208 interrupted refreshes must age the whole panel uniformly; windowed
  // refreshes leave hard color bands as pigments continue settling.
  writeFrame(bus, fb);
  refresh(bus, 0, 0, PANEL_WIDTH, PANEL_HEIGHT);

  memcpy(_prevFrame, fb, LOGICAL_BUF);
  _lastFrameValid = true;
  if (turnOff) deepSleep(bus);
}

void Ed2208M5Driver::deepSleep(EpdBus& bus) { powerOff(bus); }

PanelDriver& ed2208M5Driver() {
  static Ed2208M5Driver instance;
  return instance;
}

}  // namespace freeink
