#include "Gdey075t7Driver.h"

#include <BoardConfig.h>

namespace freeink {

Gdey075t7Driver::Gdey075t7Driver()
    : _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Gdey075t7Driver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 2000000;
}

PanelGeometry Gdey075t7Driver::geometry() const {
  return {_w, _h, _wb, _bufferSize};
}

void Gdey075t7Driver::initDisplay(EpdBus& bus) {
  bus.reset();

  // PANEL SETTING (0x00)
  bus.cmd(0x00);
  bus.data(0x1F);  // KW-mode OTP LUT, scan up (UD=1), shift right (SHL=1), booster on

  // POWER SETTING (0x01)
  bus.cmd(0x01);
  bus.data(0x07);  // enable internal VGH/VGL
  bus.data(0x07);  // VGH=20V, VGL=-20V
  bus.data(0x3F);  // VDH=15V
  bus.data(0x3F);  // VDL=-15V
  bus.data(0x09);  // VDHR=4.2V

  // Booster Soft Start (0x06)
  bus.cmd(0x06);
  bus.data(0x17);
  bus.data(0x17);
  bus.data(0x28);
  bus.data(0x17);

  // Resolution TRES (0x61)
  bus.cmd(0x61);
  bus.data(static_cast<uint8_t>((_w >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_w & 0xFF));
  bus.data(static_cast<uint8_t>((_h >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_h & 0xFF));

  // DUSPI (0x15)
  bus.cmd(0x15);
  bus.data(0x00);  // disabled

  // VCOM AND DATA INTERVAL SETTING (0x50)
  bus.cmd(0x50);
  bus.data(0x29);  // LUTKW, N2OCP: copy new to old
  bus.data(0x07);  // CDI 10 hsynch

  // TCON SETTING (0x60)
  bus.cmd(0x60);
  bus.data(0x22);  // S2G, G2S

  // PWS (0xE3)
  bus.cmd(0xE3);
  bus.data(0x22);  // VCOM 2 line period, Source 2 * 660ns

  _screenOn = false;
}

void Gdey075t7Driver::powerOn(EpdBus& bus) {
  if (!_screenOn) {
    bus.cmd(0x04);  // Power ON
    bus.waitBusy(" GDEY075T7_PON");
    _screenOn = true;
  }
}

void Gdey075t7Driver::powerOff(EpdBus& bus) {
  if (_screenOn) {
    bus.cmd(0x02);  // Power OFF
    bus.waitBusy(" GDEY075T7_POF");
    _screenOn = false;
  }
}

void Gdey075t7Driver::begin(EpdBus& bus) {
  initDisplay(bus);
  _initialized = true;
  _firstRefresh = true;
}

void Gdey075t7Driver::deepSleep(EpdBus& bus) {
  powerOff(bus);
  bus.cmd(0x07);  // Deep sleep
  bus.data(0xA5);
  _initialized = false;
}

void Gdey075t7Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  if (!_initialized) {
    initDisplay(bus);
    _initialized = true;
  }

  const bool full = (mode == RefreshMode::Full || mode == RefreshMode::Half || _firstRefresh);

  if (full) {
    // Full update sequence:
    bus.cmd(0x00);
    bus.data(0x1F);  // OTP LUT, scan up

    bus.cmd(0xE0);   // CCSET
    bus.data(0x02);  // TSFIX
    bus.cmd(0xE5);   // TSSET
    bus.data(0x5A);  // 90 deg C for fast full refresh OTP waveform

    bus.sendPlane(0x10, fb, _bufferSize);
    bus.sendPlane(0x13, fb, _bufferSize);

    powerOn(bus);

    bus.cmd(0x12);  // Display Refresh
    bus.waitRefreshComplete(" GDEY075T7_Full");

    _firstRefresh = false;
  } else {
    // Fast partial update sequence:
    bus.cmd(0xE0);   // CCSET
    bus.data(0x02);  // TSFIX
    bus.cmd(0xE5);   // TSSET
    bus.data(0x6E);  // 110 deg C for fast partial

    if (prev != nullptr) {
      bus.sendPlane(0x10, prev, _bufferSize);
    }
    bus.sendPlane(0x13, fb, _bufferSize);

    powerOn(bus);

    bus.cmd(0x12);  // Display Refresh
    bus.waitRefreshComplete(" GDEY075T7_Fast");
  }

  if (turnOff) {
    powerOff(bus);
  }
}

PanelDriver& gdey075t7Driver() {
  static Gdey075t7Driver instance;
  return instance;
}

}  // namespace freeink
