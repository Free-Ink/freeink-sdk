#include "Ssd1677Driver.h"

#include <BoardConfig.h>

#include <vector>

#include "../lut/Ssd1677Luts.h"

namespace freeink {
namespace {

// SSD1677 command set.
constexpr uint8_t CMD_SOFT_RESET = 0x12;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x0C;
constexpr uint8_t CMD_DRIVER_OUTPUT_CONTROL = 0x01;
constexpr uint8_t CMD_BORDER_WAVEFORM = 0x3C;
constexpr uint8_t CMD_TEMP_SENSOR_CONTROL = 0x18;
constexpr uint8_t CMD_DATA_ENTRY_MODE = 0x11;
constexpr uint8_t CMD_SET_RAM_X_RANGE = 0x44;
constexpr uint8_t CMD_SET_RAM_Y_RANGE = 0x45;
constexpr uint8_t CMD_SET_RAM_X_COUNTER = 0x4E;
constexpr uint8_t CMD_SET_RAM_Y_COUNTER = 0x4F;
constexpr uint8_t CMD_WRITE_RAM_BW = 0x24;
constexpr uint8_t CMD_WRITE_RAM_RED = 0x26;
constexpr uint8_t CMD_AUTO_WRITE_BW_RAM = 0x46;
constexpr uint8_t CMD_AUTO_WRITE_RED_RAM = 0x47;
constexpr uint8_t CMD_DISPLAY_UPDATE_CTRL1 = 0x21;
constexpr uint8_t CMD_DISPLAY_UPDATE_CTRL2 = 0x22;
constexpr uint8_t CMD_MASTER_ACTIVATION = 0x20;
constexpr uint8_t CTRL1_NORMAL = 0x00;
constexpr uint8_t CTRL1_BYPASS_RED = 0x40;
constexpr uint8_t CMD_WRITE_LUT = 0x32;
constexpr uint8_t CMD_GATE_VOLTAGE = 0x03;
constexpr uint8_t CMD_SOURCE_VOLTAGE = 0x04;
constexpr uint8_t CMD_WRITE_VCOM = 0x2C;
constexpr uint8_t CMD_WRITE_TEMP = 0x1A;
constexpr uint8_t CMD_DEEP_SLEEP = 0x10;

#if defined(FREEINK_DISPLAY_FLIPPED) || defined(FLIPPED)
constexpr uint8_t DRIVER_OUTPUT_SCAN = 0x03;  // vertically flipped
#else
constexpr uint8_t DRIVER_OUTPUT_SCAN = 0x02;  // SM=1 interlaced, TB=0
#endif

}  // namespace

const Ssd1677Config& ssd1677DefaultConfig() {
  // Xteink X4 / GDEQ0426T82 defaults. A board variant supplies its own struct
  // (e.g. de-link could bump the last booster byte for contrast, set its own
  // scan direction, or swap in tuned grayscale LUTs) without touching the driver.
  static const Ssd1677Config cfg = {
      {0xAE, 0xC7, 0xC3, 0xC0, 0x40},  // booster soft-start
      DRIVER_OUTPUT_SCAN,
      0x5A,  // HALF refresh temperature
      lut_grayscale,
      lut_grayscale_revert,
  };
  return cfg;
}

Ssd1677Driver::Ssd1677Driver(const Ssd1677Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Ssd1677Driver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 40000000;
}

PanelGeometry Ssd1677Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

void Ssd1677Driver::begin(EpdBus& bus) {
  bus.reset();
  initController(bus);
}

void Ssd1677Driver::initController(EpdBus& bus) {
  constexpr uint8_t TEMP_SENSOR_INTERNAL = 0x80;

  bus.cmd(CMD_SOFT_RESET);
  bus.waitBusy(" CMD_SOFT_RESET");

  bus.cmd(CMD_TEMP_SENSOR_CONTROL);
  bus.data(TEMP_SENSOR_INTERNAL);

  // Booster soft-start (device-tunable via config).
  bus.cmd(CMD_BOOSTER_SOFT_START);
  for (uint8_t b : _cfg.booster) {
    bus.data(b);
  }

  // Driver output control: display height + scan direction.
  bus.cmd(CMD_DRIVER_OUTPUT_CONTROL);
  bus.data((_h - 1) % 256);
  bus.data((_h - 1) / 256);
  bus.data(_cfg.driverOutputScan);

  bus.cmd(CMD_BORDER_WAVEFORM);
  bus.data(0x01);

  setRamArea(bus, 0, 0, _w, _h);

  bus.cmd(CMD_AUTO_WRITE_BW_RAM);
  bus.data(0xF7);
  bus.waitBusy(" CMD_AUTO_WRITE_BW_RAM");

  bus.cmd(CMD_AUTO_WRITE_RED_RAM);
  bus.data(0xF7);
  bus.waitBusy(" CMD_AUTO_WRITE_RED_RAM");

  _isScreenOn = false;
}

void Ssd1677Driver::setRamArea(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  constexpr uint8_t DATA_ENTRY_X_INC_Y_DEC = 0x01;

  // Gates are physically reversed on this panel.
  y = _h - y - h;

  bus.cmd(CMD_DATA_ENTRY_MODE);
  bus.data(DATA_ENTRY_X_INC_Y_DEC);

  bus.cmd(CMD_SET_RAM_X_RANGE);
  bus.data(x % 256);
  bus.data(x / 256);
  bus.data((x + w - 1) % 256);
  bus.data((x + w - 1) / 256);

  bus.cmd(CMD_SET_RAM_Y_RANGE);
  bus.data((y + h - 1) % 256);
  bus.data((y + h - 1) / 256);
  bus.data(y % 256);
  bus.data(y / 256);

  bus.cmd(CMD_SET_RAM_X_COUNTER);
  bus.data(x % 256);
  bus.data(x / 256);

  bus.cmd(CMD_SET_RAM_Y_COUNTER);
  bus.data((y + h - 1) % 256);
  bus.data((y + h - 1) / 256);
}

void Ssd1677Driver::writeRam(EpdBus& bus, uint8_t ramCmd, const uint8_t* data, uint32_t size) {
  bus.cmd(ramCmd);
  bus.data(data, static_cast<uint16_t>(size));
}

void Ssd1677Driver::refresh(EpdBus& bus, RefreshMode mode, bool turnOff) {
  bus.cmd(CMD_DISPLAY_UPDATE_CTRL1);
  bus.data((mode == RefreshMode::Fast) ? CTRL1_NORMAL : CTRL1_BYPASS_RED);

  uint8_t displayMode = 0x00;
  if (!_isScreenOn) {
    _isScreenOn = true;
    displayMode |= 0xC0;  // CLOCK_ON | ANALOG_ON
  }
  if (turnOff) {
    _isScreenOn = false;
    displayMode |= 0x03;  // ANALOG_OFF_PHASE | CLOCK_OFF
  }

  if (mode == RefreshMode::Full) {
    displayMode |= 0x34;
  } else if (mode == RefreshMode::Half) {
    bus.cmd(CMD_WRITE_TEMP);
    bus.data(_cfg.halfRefreshTemp);
    displayMode |= 0xD4;
  } else {  // Fast
    displayMode |= _customLutActive ? 0x0C : 0x1C;
  }

  bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
  bus.data(displayMode);
  bus.cmd(CMD_MASTER_ACTIVATION);
  bus.waitBusy("refresh");
}

void Ssd1677Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  // Force half refresh if the panel is asleep (avoids a slow cold full refresh).
  if (!_isScreenOn && !turnOff) {
    mode = RefreshMode::Half;
  }

  // A grayscale frame leaves the LUT loaded; revert to clean B/W first (main
  // fixed the upstream dead-revert bug — this actually runs the revert waveform).
  if (_inGrayscaleMode) {
    grayscaleRevert(bus, fb);
  }

  setRamArea(bus, 0, 0, _w, _h);

  if (mode != RefreshMode::Fast) {
    writeRam(bus, CMD_WRITE_RAM_BW, fb, _bufferSize);
    writeRam(bus, CMD_WRITE_RAM_RED, fb, _bufferSize);
  } else {
    writeRam(bus, CMD_WRITE_RAM_BW, fb, _bufferSize);
    // Dual-buffer: RED holds the previous frame for the differential compare.
    // Single-buffer (prev == nullptr): RED already holds it from last refresh.
    if (prev != nullptr) {
      writeRam(bus, CMD_WRITE_RAM_RED, prev, _bufferSize);
    }
  }

  refresh(bus, mode, turnOff);

  // Single-buffer: sync RED with the just-shown frame for the next fast diff.
  if (prev == nullptr) {
    setRamArea(bus, 0, 0, _w, _h);
    writeRam(bus, CMD_WRITE_RAM_RED, fb, _bufferSize);
  }
}

void Ssd1677Driver::displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y,
                                  uint16_t w, uint16_t h, bool turnOff) {
  if (x + w > _w || y + h > _h) return;
  if (x % 8 != 0 || w % 8 != 0) return;  // window must be byte-aligned
  if (!fb) return;

  // A windowed update can't coexist with grayscale content on the rest of the
  // screen — drive it back to clean BW first (upstream parity; not a bare flag
  // clear, which would leave the gray residue unreverted).
  if (_inGrayscaleMode) {
    grayscaleRevert(bus, fb);
  }

  const uint16_t windowWidthBytes = w / 8;
  const uint32_t windowBufferSize = static_cast<uint32_t>(windowWidthBytes) * h;

  std::vector<uint8_t> windowBuffer(windowBufferSize);
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t srcY = y + row;
    const uint32_t srcOffset = static_cast<uint32_t>(srcY) * _wb + (x / 8);
    const uint32_t dstOffset = static_cast<uint32_t>(row) * windowWidthBytes;
    memcpy(&windowBuffer[dstOffset], &fb[srcOffset], windowWidthBytes);
  }

  setRamArea(bus, x, y, w, h);
  writeRam(bus, CMD_WRITE_RAM_BW, windowBuffer.data(), windowBufferSize);

  if (prev != nullptr) {
    std::vector<uint8_t> previousWindow(windowBufferSize);
    for (uint16_t row = 0; row < h; row++) {
      const uint16_t srcY = y + row;
      const uint32_t srcOffset = static_cast<uint32_t>(srcY) * _wb + (x / 8);
      const uint32_t dstOffset = static_cast<uint32_t>(row) * windowWidthBytes;
      memcpy(&previousWindow[dstOffset], &prev[srcOffset], windowWidthBytes);
    }
    writeRam(bus, CMD_WRITE_RAM_RED, previousWindow.data(), windowBufferSize);
  }

  refresh(bus, RefreshMode::Fast, turnOff);

  if (prev == nullptr) {
    setRamArea(bus, x, y, w, h);
    writeRam(bus, CMD_WRITE_RAM_RED, windowBuffer.data(), windowBufferSize);
  }
}

void Ssd1677Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  if (!lsb) return;
  setRamArea(bus, 0, 0, _w, _h);
  writeRam(bus, CMD_WRITE_RAM_BW, lsb, _bufferSize);
}

void Ssd1677Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (!msb) return;
  setRamArea(bus, 0, 0, _w, _h);
  writeRam(bus, CMD_WRITE_RAM_RED, msb, _bufferSize);
}

void Ssd1677Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                             uint16_t numRows) {
  if (!rows || numRows == 0) return;
  const uint8_t ramCmd = (plane == GrayPlane::Lsb) ? CMD_WRITE_RAM_BW : CMD_WRITE_RAM_RED;
  setRamArea(bus, 0, yStart, _w, numRows);
  bus.cmd(ramCmd);
  bus.data(rows, static_cast<uint16_t>(static_cast<uint32_t>(numRows) * _wb));
}

void Ssd1677Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)fb;
  // Differential mode leaves the LUT loaded (reverted before the next BW turn);
  // factory absolute mode self-cleans.
  _inGrayscaleMode = !factoryMode;

  const unsigned char* selectedLut = lut;
  if (selectedLut == nullptr) {
    selectedLut = factoryMode ? lut_factory_quality : _cfg.grayLut;
  }
  setCustomLut(bus, true, selectedLut);

  if (factoryMode) {
    // Explicit, self-contained power cycle for 4-level absolute grayscale.
    // Reset CTRL1 to normal — a prior HALF leaves BYPASS_RED set, which would
    // ignore RED RAM and break 4-level grayscale.
    bus.cmd(CMD_DISPLAY_UPDATE_CTRL1);
    bus.data(CTRL1_NORMAL);
    bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
    bus.data(0xC7);  // CLOCK_ON|ANALOG_ON|DISPLAY_START|ANALOG_OFF|CLOCK_OFF
    bus.cmd(CMD_MASTER_ACTIVATION);
    bus.waitBusy("factory_gray");
    _isScreenOn = false;  // 0xC7 always powers down after the update
  } else {
    refresh(bus, RefreshMode::Fast, turnOff);
  }

  setCustomLut(bus, false, nullptr);
}

void Ssd1677Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw) return;
  setRamArea(bus, 0, 0, _w, _h);
  writeRam(bus, CMD_WRITE_RAM_RED, bw, _bufferSize);
  // The restored BW frame in RED RAM *is* the clean differential baseline for the
  // next BW page turn, so no revert is needed — clear the flag (upstream parity).
  // Leaving it set fires a spurious grayscaleRevert() on the next refresh, which
  // drives the revert LUT against this baseline and leaves accumulating ghost.
  _inGrayscaleMode = false;
}

void Ssd1677Driver::grayscaleRevert(EpdBus& bus, const uint8_t* fb) {
  (void)fb;
  if (!_inGrayscaleMode) return;
  _inGrayscaleMode = false;
  setCustomLut(bus, true, _cfg.grayRevertLut);
  refresh(bus, RefreshMode::Fast, false);
  setCustomLut(bus, false, nullptr);
}

void Ssd1677Driver::setCustomLut(EpdBus& bus, bool enabled, const unsigned char* data) {
  if (!enabled) {
    _customLutActive = false;
    return;
  }

  // First 105 bytes: VS + TP/RP + frame rate.
  bus.cmd(CMD_WRITE_LUT);
  for (uint16_t i = 0; i < 105; i++) {
    bus.data(pgm_read_byte(&data[i]));
  }

  bus.cmd(CMD_GATE_VOLTAGE);  // VGH
  bus.data(pgm_read_byte(&data[105]));

  bus.cmd(CMD_SOURCE_VOLTAGE);  // VSH1, VSH2, VSL
  bus.data(pgm_read_byte(&data[106]));
  bus.data(pgm_read_byte(&data[107]));
  bus.data(pgm_read_byte(&data[108]));

  bus.cmd(CMD_WRITE_VCOM);  // VCOM
  bus.data(pgm_read_byte(&data[109]));

  _customLutActive = true;
}

void Ssd1677Driver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_DISPLAY_UPDATE_CTRL1);
    bus.data(CTRL1_BYPASS_RED);
    bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
    bus.data(0x03);  // ANALOG_OFF_PHASE | CLOCK_OFF
    bus.cmd(CMD_MASTER_ACTIVATION);
    bus.waitBusy(" display power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0x01);
}

// Per-board waveform/LUT injection: a board supplies its own SSD1677 config
// (booster, scan, grayscale LUTs) without editing this driver — define
// `const Ssd1677Config& yourConfig();` in namespace freeink and build with
// -DFREEINK_SSD1677_CONFIG=yourConfig. Resolution is orthogonal: every driver,
// including X3, takes its geometry from the active BoardProfile.
#ifdef FREEINK_SSD1677_CONFIG
const Ssd1677Config& FREEINK_SSD1677_CONFIG();
static const Ssd1677Config& ssd1677ActiveConfig() { return FREEINK_SSD1677_CONFIG(); }
#else
static const Ssd1677Config& ssd1677ActiveConfig() { return ssd1677DefaultConfig(); }
#endif

PanelDriver& ssd1677Driver() {
  static Ssd1677Driver instance(ssd1677ActiveConfig());
  return instance;
}

}  // namespace freeink
