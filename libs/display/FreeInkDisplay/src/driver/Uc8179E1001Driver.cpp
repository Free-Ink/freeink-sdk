#include "Uc8179E1001Driver.h"

#include <BoardConfig.h>
#if FREEINK_DRIVER_UC8179_E1001

#include <Arduino.h>

#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

namespace freeink {
namespace {

constexpr uint8_t CMD_PSR = 0x00;
constexpr uint8_t CMD_PWR = 0x01;
constexpr uint8_t CMD_POF = 0x02;
constexpr uint8_t CMD_PON = 0x04;
constexpr uint8_t CMD_BTST = 0x06;
constexpr uint8_t CMD_DSLP = 0x07;
constexpr uint8_t CMD_DTM1 = 0x10;  // OLD plane (KW mode)
constexpr uint8_t CMD_DRF = 0x12;
constexpr uint8_t CMD_DTM2 = 0x13;  // NEW plane (KW mode)
constexpr uint8_t CMD_DUSPI = 0x15;
constexpr uint8_t CMD_CDI = 0x50;
constexpr uint8_t CMD_TCON = 0x60;
constexpr uint8_t CMD_TRES = 0x61;
constexpr uint8_t CMD_PTL = 0x90;
constexpr uint8_t CMD_PTIN = 0x91;
constexpr uint8_t CMD_PTOUT = 0x92;
constexpr uint8_t CMD_CCSET = 0xE0;
constexpr uint8_t CMD_PWS = 0xE3;
constexpr uint8_t CMD_TSSET = 0xE5;
constexpr uint8_t CMD_LUTC = 0x20;  // VCOM, then 0x21 WW, 0x22 KW, 0x23 WK, 0x24 KK, 0x25 border

constexpr uint8_t TSSET_FULL = 0x5A;
constexpr uint8_t TSSET_PARTIAL = 0x6E;

// Register waveform frames per drive phase (about 20 ms each): 16 is about 770 ms per update.
// Fewer frames are faster but leave more traces (12 already did on a real unit).
constexpr uint8_t kFastFrames = 16;

bool g_fast = false;         // uc8179E1001SetFastWaveform
bool g_modeChanged = false;  // the next fast update re-initialises with the other waveform

class Uc8179E1001Driver : public PanelDriver {
 public:
  // Geometry comes from the board profile (800x480 on the E1001).
  Uc8179E1001Driver()
      : _w(BoardConfig::ACTIVE.displayWidth),
        _h(BoardConfig::ACTIVE.displayHeight),
        _wb(BoardConfig::ACTIVE.displayWidth / 8),
        _bytes(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

  const char* controllerName() const override { return "UC8179"; }
  uint32_t spiHz() const override {
    return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 4000000;
  }
  BusyPolarity busyPolarity() const override { return BusyPolarity::UcIdleHigh; }
  PanelGeometry geometry() const override { return {_w, _h, _wb, _bytes}; }

  void begin(EpdBus& bus) override {
    bus.reset(100);
    bus.waitBusy(" e1001_reset");
    _powered = false;
    _partialMode = false;
    _oldValid = false;
  }

  void deepSleep(EpdBus& bus) override {
    powerOff(bus);
    bus.cmd(CMD_DSLP);
    bus.data(0xA5);
    _oldValid = false;  // RAM contents are lost in deep sleep
  }

  void requestResync(uint8_t) override { _oldValid = false; }
  void skipInitialResync() override {}

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t*, RefreshMode mode, bool turnOff) override {
    if (!fb) return;
    if (mode == RefreshMode::Fast && _oldValid && _shown) {
      // Send only the part of the screen that changed; the panel stays powered between updates
      // (deepSleep and the full refresh power it down).
      uint16_t x, y, w, h;
      if (!dirtyBox(fb, x, y, w, h)) return;
      partial(bus, fb, x, y, w, h);
      remember(fb);
      return;
    }
    full(bus, fb);
    remember(fb);
    if (turnOff) powerOff(bus);
  }

  void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y, uint16_t w,
                     uint16_t h, bool turnOff) override {
    if (!fb) return;
    if (!_oldValid) {
      display(bus, fb, prev, RefreshMode::Full, turnOff);
      return;
    }
    partial(bus, fb, x, y, w, h);
    remember(fb);
  }

 private:
  const uint16_t _w;
  const uint16_t _h;
  const uint16_t _wb;
  const uint32_t _bytes;
  bool _powered = false;
  bool _partialMode = false;
  bool _oldValid = false;
  bool _shown = false;          // _shadow holds the frame the panel is showing
  uint8_t* _shadow = nullptr;   // copy of the last frame sent, to find what changed

  void remember(const uint8_t* fb) {
    if (!_shadow) {
      _shadow = static_cast<uint8_t*>(heap_caps_malloc(_bytes, MALLOC_CAP_SPIRAM));
      if (!_shadow) _shadow = static_cast<uint8_t*>(malloc(_bytes));
    }
    if (_shadow) {
      memcpy(_shadow, fb, _bytes);
      _shown = true;
    }
  }

  // Bounding box (byte-aligned columns) of what differs from the last frame; false if nothing did.
  bool dirtyBox(const uint8_t* fb, uint16_t& x, uint16_t& y, uint16_t& w, uint16_t& h) {
    int y0 = -1, y1 = -1, b0 = _wb, b1 = -1;
    for (uint16_t r = 0; r < _h; r++) {
      const uint8_t* a = fb + static_cast<uint32_t>(r) * _wb;
      const uint8_t* b = _shadow + static_cast<uint32_t>(r) * _wb;
      if (memcmp(a, b, _wb) == 0) continue;
      if (y0 < 0) y0 = r;
      y1 = r;
      int i = 0;
      while (a[i] == b[i]) i++;
      int j = _wb - 1;
      while (a[j] == b[j]) j--;
      if (i < b0) b0 = i;
      if (j > b1) b1 = j;
    }
    if (y0 < 0) return false;
    x = static_cast<uint16_t>(b0 * 8);
    w = static_cast<uint16_t>((b1 - b0 + 1) * 8);
    y = static_cast<uint16_t>(y0);
    h = static_cast<uint16_t>(y1 - y0 + 1);
    return true;
  }

  // Register waveform (GxEPD2 GDEY075T7 recipe, shortened): charge-balancing pre-phase, then the drive.
  void writeLut(EpdBus& bus, uint8_t reg, uint8_t levels) {
    bus.cmd(reg);
    for (int i = 0; i < 42; i++) {
      uint8_t b = 0;
      if (i == 0) b = levels;
      else if (i == 1 || i == 3) b = kFastFrames;
      else if (i == 5) b = 1;
      bus.data(b);
    }
  }
  void loadRegisterWaveform(EpdBus& bus) {
    bus.cmd(CMD_PSR);
    bus.data(0x3F);  // KW mode, LUT from registers
    bus.cmd(CMD_CDI);
    bus.data(0x39);
    bus.data(0x07);
    writeLut(bus, CMD_LUTC, 0x00);
    writeLut(bus, 0x21, 0x00);
    writeLut(bus, 0x22, 0x5A);
    writeLut(bus, 0x23, 0x84);
    writeLut(bus, 0x24, 0x00);
    writeLut(bus, 0x25, 0x00);
  }

  void initCommon(EpdBus& bus) {
    bus.cmd(CMD_PSR);
    bus.data(0x1F);  // KW mode, OTP LUT, scan up, shift right
    bus.cmd(CMD_PWR);
    bus.data(0x07);
    bus.data(0x07);
    bus.data(0x3F);
    bus.data(0x3F);
    bus.data(0x09);
    bus.cmd(CMD_BTST);
    bus.data(0x17);
    bus.data(0x17);
    bus.data(0x28);
    bus.data(0x17);
    bus.cmd(CMD_TRES);
    bus.data(_w >> 8);
    bus.data(_w & 0xFF);
    bus.data(_h >> 8);
    bus.data(_h & 0xFF);
    bus.cmd(CMD_DUSPI);
    bus.data(0x00);
    bus.cmd(CMD_CDI);
    bus.data(0x29);
    bus.data(0x07);
    bus.cmd(CMD_TCON);
    bus.data(0x22);
    bus.cmd(CMD_PWS);
    bus.data(0x22);
  }

  void powerOn(EpdBus& bus) {
    if (_powered) return;
    bus.cmd(CMD_PON);
    bus.waitBusy(" e1001_PON");
    _powered = true;
  }

  void powerOff(EpdBus& bus) {
    if (!_powered) return;
    bus.cmd(CMD_POF);
    bus.waitBusy(" e1001_POF");
    _powered = false;
    _partialMode = false;
  }

  // Stream rows [y, y+h) x bytes [xb, xb+cols) of `fb` into plane `ramCmd`, optionally complemented.
  void streamRows(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb, uint16_t xb, uint16_t cols, uint16_t y,
                  uint16_t h, bool invert) {
    uint8_t chunk[128];  // one 800-px row per write; wider panels take several
    bus.cmd(ramCmd);
    auto txn = bus.beginTxn();
    for (uint16_t r = 0; r < h; r++) {
      const uint8_t* src = fb + static_cast<uint32_t>(y + r) * _wb + xb;
      if (invert) {
        for (uint16_t i = 0; i < cols; i += sizeof(chunk)) {
          const uint16_t n = static_cast<uint16_t>(cols - i) < sizeof(chunk) ? static_cast<uint16_t>(cols - i)
                                                                         : static_cast<uint16_t>(sizeof(chunk));
          for (uint16_t k = 0; k < n; k++) chunk[k] = static_cast<uint8_t>(~src[i + k]);
          txn.writeBytes(chunk, n);
        }
      } else {
        txn.writeBytes(src, cols);
      }
    }
  }

  void setWindow(EpdBus& bus, uint16_t xs, uint16_t y, uint16_t cols8, uint16_t h) {
    const uint16_t xe = static_cast<uint16_t>((xs + cols8 - 1) | 7);
    const uint16_t ye = static_cast<uint16_t>(y + h - 1);
    bus.cmd(CMD_PTL);
    bus.data(xs >> 8);
    bus.data(xs & 0xFF);
    bus.data(xe >> 8);
    bus.data(xe & 0xFF);
    bus.data(y >> 8);
    bus.data(y & 0xFF);
    bus.data(ye >> 8);
    bus.data(ye & 0xFF);
    bus.data(0x01);
  }

  // Flashing refresh: OLD plane = complement of the new frame, so every pixel is driven.
  void full(EpdBus& bus, const uint8_t* fb) {
    powerOff(bus);  // re-init needs the booster off; also drops partial mode
    initCommon(bus);
    powerOn(bus);
    streamRows(bus, CMD_DTM2, fb, 0, _wb, 0, _h, false);
    streamRows(bus, CMD_DTM1, fb, 0, _wb, 0, _h, true);
    bus.cmd(CMD_CCSET);
    bus.data(0x02);
    bus.cmd(CMD_TSSET);
    bus.data(TSSET_FULL);
    bus.cmd(CMD_DRF);
    bus.waitRefreshComplete(" e1001_DRF_full");
    streamRows(bus, CMD_DTM1, fb, 0, _wb, 0, _h, false);  // OLD := shown frame
    _oldValid = true;
  }

  // Fast update of one window with the built-in or the register waveform (uc8179E1001SetFastWaveform).
  void partial(EpdBus& bus, const uint8_t* fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (g_modeChanged) {  // the app switched waveform: re-initialise with the new one
      powerOff(bus);
      g_modeChanged = false;
    }
    if (!_partialMode) {
      initCommon(bus);
      if (g_fast) {
        loadRegisterWaveform(bus);
      } else {
        bus.cmd(CMD_CCSET);
        bus.data(0x02);
        bus.cmd(CMD_TSSET);
        bus.data(TSSET_PARTIAL);
      }
      powerOn(bus);
      _partialMode = true;
    }
    const uint16_t xs = x & ~7u;
    const uint16_t xe = static_cast<uint16_t>((x + w - 1) | 7);
    const uint16_t cols = static_cast<uint16_t>((xe - xs + 1) / 8);

    bus.cmd(CMD_PTIN);
    setWindow(bus, xs, y, static_cast<uint16_t>(cols * 8), h);
    streamRows(bus, CMD_DTM2, fb, xs / 8, cols, y, h, false);
    bus.cmd(CMD_DRF);
    bus.waitRefreshComplete(" e1001_DRF_part");
    bus.cmd(CMD_PTOUT);

    bus.cmd(CMD_PTIN);
    setWindow(bus, xs, y, static_cast<uint16_t>(cols * 8), h);
    streamRows(bus, CMD_DTM1, fb, xs / 8, cols, y, h, false);  // OLD := shown window
    bus.cmd(CMD_PTOUT);
  }
};

}  // namespace

PanelDriver& uc8179E1001Driver() {
  static Uc8179E1001Driver instance;
  return instance;
}

void uc8179E1001SetFastWaveform(bool fast) {
  if (fast == g_fast) return;
  g_fast = fast;
  g_modeChanged = true;
}

}  // namespace freeink

#endif  // FREEINK_DRIVER_UC8179_E1001
