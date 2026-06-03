#include "Ed2208M5Driver.h"

#include <BoardConfig.h>
#include <Wire.h>

#include <algorithm>
#include <cstring>

// Native M5Stack PaperColor (ED2208) driver, ported from the upstream
// community-sdk feat-m5-paper-color branch. See Ed2208M5Driver.h for why the
// refresh is interrupted at ~340 ms (6-color panel, white settles last).

namespace freeink {
namespace {

// Physical panel geometry (the app-facing framebuffer is 600x400 landscape).
constexpr uint16_t PANEL_WIDTH = 400;
constexpr uint16_t PANEL_HEIGHT = 600;
constexpr uint16_t REFRESH_CUTOFF_MS = 340;
constexpr uint16_t BUSY_SETTLE_MS = 20;
constexpr uint8_t FAST_CLEAN_INTERVAL = 6;
constexpr uint8_t DARK_DISPLAY_CTRL = 0x0F;
constexpr uint32_t PANEL_AREA = static_cast<uint32_t>(PANEL_WIDTH) * PANEL_HEIGHT;
constexpr uint32_t PARTIAL_AREA_LIMIT = PANEL_AREA / 4;

// --- M5PM1 PMIC (routes EPD panel power via GPIO0 / EPD_EN over internal I2C) ---
constexpr uint8_t M5PM1_ADDR = 0x6E;
constexpr uint8_t M5PM1_POWER_CONFIG_REG = 0x06;
constexpr uint8_t M5PM1_WATCHDOG_REG = 0x09;
constexpr uint8_t M5PM1_GPIO_MODE_REG = 0x10;
constexpr uint8_t M5PM1_GPIO_OUT_REG = 0x11;
constexpr uint8_t M5PM1_GPIO_DRV_REG = 0x13;
constexpr uint8_t M5PM1_GPIO_FUNC0_REG = 0x16;
constexpr uint8_t M5PM1_POWER_CHARGE_EN = 1 << 0;
constexpr uint8_t M5PM1_POWER_LDO_EN = 1 << 2;
constexpr uint8_t M5PM1_POWER_BOOST_EN = 1 << 3;
constexpr uint8_t M5PM1_GPIO0 = 1 << 0;
constexpr int M5_INTERNAL_I2C_SDA = 3;
constexpr int M5_INTERNAL_I2C_SCL = 2;
constexpr uint32_t M5_INTERNAL_I2C_FREQ = 100000;

void m5Pm1WriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(M5PM1_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

bool m5Pm1ReadReg(uint8_t reg, uint8_t* value) {
  Wire.beginTransmission(M5PM1_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(M5PM1_ADDR, static_cast<uint8_t>(1)) != 1) return false;
  *value = Wire.read();
  return true;
}

void m5Pm1UpdateReg(uint8_t reg, uint8_t clearMask, uint8_t setMask) {
  uint8_t value = 0;
  if (m5Pm1ReadReg(reg, &value)) {
    m5Pm1WriteReg(reg, static_cast<uint8_t>((value & ~clearMask) | setMask));
  }
}

}  // namespace

uint32_t Ed2208M5Driver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 4000000;
}

PanelGeometry Ed2208M5Driver::geometry() const { return {LOGICAL_W, LOGICAL_H, LOGICAL_WB, LOGICAL_BUF}; }
int8_t Ed2208M5Driver::spiMiso() const { return BoardConfig::ACTIVE.sd.miso; }
int8_t Ed2208M5Driver::coCs() const { return BoardConfig::ACTIVE.sd.cs; }

void Ed2208M5Driver::enablePmicPower() {
  Wire.begin(M5_INTERNAL_I2C_SDA, M5_INTERNAL_I2C_SCL, M5_INTERNAL_I2C_FREQ);
  Wire.setTimeOut(4);
  m5Pm1WriteReg(M5PM1_WATCHDOG_REG, 0x00);
  m5Pm1UpdateReg(M5PM1_POWER_CONFIG_REG, 0, M5PM1_POWER_CHARGE_EN | M5PM1_POWER_LDO_EN | M5PM1_POWER_BOOST_EN);
  // EPD_EN is M5PM1 GPIO0.
  m5Pm1UpdateReg(M5PM1_GPIO_FUNC0_REG, M5PM1_GPIO0, 0);
  m5Pm1UpdateReg(M5PM1_GPIO_MODE_REG, 0, M5PM1_GPIO0);
  m5Pm1UpdateReg(M5PM1_GPIO_DRV_REG, M5PM1_GPIO0, 0);
  m5Pm1UpdateReg(M5PM1_GPIO_OUT_REG, 0, M5PM1_GPIO0);
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

  bus.beginTxn();
  for (size_t i = 0; i < sizeof(initCommands);) {
    const uint8_t command = initCommands[i++];
    const uint8_t length = initCommands[i++];
    waitBusy(bus);
    bus.rawCmd(command);
    for (uint8_t j = 0; j < length; ++j) {
      bus.rawData(initCommands[i++]);
    }
  }
  waitBusy(bus);
  bus.rawCmd(0x61);
  bus.rawData(static_cast<uint8_t>((PANEL_WIDTH >> 8) & 0xFF));
  bus.rawData(static_cast<uint8_t>(PANEL_WIDTH & 0xFF));
  bus.rawData(static_cast<uint8_t>((PANEL_HEIGHT >> 8) & 0xFF));
  bus.rawData(static_cast<uint8_t>(PANEL_HEIGHT & 0xFF));
  bus.endTxn();
}

void Ed2208M5Driver::begin(EpdBus& bus) {
  enablePmicPower();
  bus.reset();
  initController(bus);
  memset(_prevFrame, 0xFF, LOGICAL_BUF);
  _panelPowerOn = false;
  _lastFrameValid = false;
  _fastRefreshesSinceFullPanel = 0;
  _completeNextRefresh = false;
}

void Ed2208M5Driver::writeFrame(EpdBus& bus, const uint8_t* fb) {
  // Interrupted refreshes settle dark with this polarity: logical white -> physical
  // black, logical black -> physical white.
  static constexpr uint8_t EPD_WHITE = 0x0;
  static constexpr uint8_t EPD_BLACK = 0x1;
  uint8_t packedRow[PANEL_WIDTH / 2];

  bus.beginTxn();
  bus.rawCmd(0x10);
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
    bus.rawWriteBytes(packedRow, sizeof(packedRow));
  }
  bus.endTxn();
}

void Ed2208M5Driver::setPartialWindow(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
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
  bus.rawCmd(0x83);
  for (uint8_t v : window) bus.rawData(v);
}

void Ed2208M5Driver::powerOn(EpdBus& bus) {
  if (_panelPowerOn) return;
  bus.beginTxn();
  bus.rawCmd(0x04);
  waitBusy(bus);
  bus.endTxn();
  _panelPowerOn = true;
}

void Ed2208M5Driver::powerOff(EpdBus& bus) {
  if (!_panelPowerOn) return;
  bus.beginTxn();
  bus.rawCmd(0x02);
  bus.rawData(0x00);
  waitBusy(bus);
  bus.endTxn();
  _panelPowerOn = false;
}

void Ed2208M5Driver::interruptRefresh(EpdBus& bus) {
  delay(REFRESH_CUTOFF_MS);  // abort the OTP waveform before white settles
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

  bus.beginTxn();
  bus.rawCmd(0x06);
  bus.rawData(0x6F);
  bus.rawData(0x1F);
  bus.rawData(0x17);
  bus.rawData(0x27);
  setPartialWindow(bus, dirtyX, dirtyY, dirtyW, dirtyH);
  bus.rawCmd(0x50);
  // Complete refresh uses the vendor VCOM/CDI for full contrast; the fast
  // interrupt path keeps the dark-hack value.
  bus.rawData(completeWaveform ? 0x3F : DARK_DISPLAY_CTRL);
  bus.rawCmd(0x12);
  bus.rawData(0x00);
  bus.endTxn();

  if (completeWaveform) {
    waitBusy(bus);  // run the full OTP waveform to completion
    bus.beginTxn();
    bus.rawCmd(0x02);  // POWER_OFF
    bus.rawData(0x00);
    waitBusy(bus);
    bus.endTxn();
    _panelPowerOn = false;
  } else {
    interruptRefresh(bus);
  }
}

void Ed2208M5Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  if (!fb) return;

  uint16_t refreshX = 0, refreshY = 0, refreshW = PANEL_WIDTH, refreshH = PANEL_HEIGHT;
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

  if (!forceFullPanelRefresh && _lastFrameValid && _fastRefreshesSinceFullPanel < FAST_CLEAN_INTERVAL) {
    const uint32_t dirtyArea = static_cast<uint32_t>(dirtyW) * dirtyH;
    if (dirtyArea <= PARTIAL_AREA_LIMIT) {
      refreshX = dirtyX;
      refreshY = dirtyY;
      refreshW = dirtyW;
      refreshH = dirtyH;
    }
  }

  // ED2208 0x10 expects a coherent full-frame upload; only the refresh window is limited.
  writeFrame(bus, fb);
  refresh(bus, refreshX, refreshY, refreshW, refreshH);

  if (refreshX == 0 && refreshY == 0 && refreshW == PANEL_WIDTH && refreshH == PANEL_HEIGHT) {
    _fastRefreshesSinceFullPanel = 0;
  } else if (_fastRefreshesSinceFullPanel < 0xFF) {
    ++_fastRefreshesSinceFullPanel;
  }

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
