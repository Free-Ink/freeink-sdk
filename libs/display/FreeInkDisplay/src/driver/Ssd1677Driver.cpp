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

constexpr uint8_t DRIVER_OUTPUT_SCAN = 0x02;  // SM=1 interlaced, TB=0 (base)
constexpr uint8_t SCAN_TB_FLIP = 0x01;        // OR into the scan byte for mirrorY

}  // namespace

const Ssd1677Config& ssd1677DefaultConfig() {
  // Xteink X4 / GDEQ0426T82 defaults. The stock X4 firmware's B/W update paths
  // use absolute SSD1677 sequences rather than the incremental 0x1C assembly:
  // INIT:  0C=AE C7 C3 C0 80, 3C=80
  // FULL:  3C=C0, 22=F7, 20, ~1800 ms
  // HALF:  3C=C0, 1A=5A, 22=D7, 20
  // PART:  3C=C0, 22=FC, 20, ~500 ms
  // Keep those as the default X4 path; weaker partial selection shows heavy
  // ghosting on some panels.
  static const Ssd1677Config cfg = {
      {0xAE, 0xC7, 0xC3, 0xC0, 0x80},  // booster soft-start
      DRIVER_OUTPUT_SCAN,
      0x80,  // borderWaveformInit: stock X4 init border
      0x5A,  // HALF refresh temperature
      lut_grayscale,
      lut_grayscale_revert,
      0xF7,  // fullSeqOverride: stock X4 full update sequence
      0xFC,  // fastSeqOverride: stock X4 partial update sequence
      0xD7,  // halfSeqOverride: stock X4 warmed/full-clean update sequence
      0xC0,  // borderWaveformFull: stock X4 border
      0xC0,  // borderWaveformFast: stock X4 border
      0xC0,  // borderWaveformHalf: stock X4 border
  };
  return cfg;
}

// Seeed Sticky. Same SSD1677 controller, resolution, and RAM polarity as the X4
// (Seeed's driver uses "1bpp MSB, 0xFF=white", bit1=white — identical to the SDK
// framebuffer). The one real difference is the waveform: the X4's fast update
// sequence (0x1C) does NOT select this panel's partial/DU waveform — it runs the
// full OTP waveform every refresh (~1.7s, UI unusably slow). Seeed's own SSD1677
// driver uses 0x22 = 0xF7 (full) / 0xFF (partial); those load temperature and
// select the DU waveform. Supplied here as per-board sequences — tune here
// (booster, LUTs, sequences), never in the driver body.
static const Ssd1677Config& ssd1677StickyConfig() {
  static const Ssd1677Config cfg = {
      {0xAE, 0xC7, 0xC3, 0xC0, 0x80},  // booster soft-start (matches Seeed's panel driver)
      DRIVER_OUTPUT_SCAN,
      0x01,  // borderWaveformInit: vendor FULL/partial-clear border
      0x5A,  // halfRefreshTemp (unused once fullSeqOverride loads temperature itself)
      lut_grayscale_sticky,         // own copy: voltage tail is per-module, tune there
      lut_grayscale_revert_sticky,  // (see Ssd1677Luts.h), never in the shared X4 LUT
      0xF7,  // fullSeqOverride: vendor FULL update sequence
      0xFF,  // fastSeqOverride: vendor PARTIAL/DU update sequence (the actual fast path)
      0x00,  // halfSeqOverride: use fullSeqOverride
      0x01,  // borderWaveformFull: vendor FULL/partial-clear border
      0x80,  // borderWaveformFast: vendor PARTIAL/DU border (stops the dark edge ring)
      0x00,  // borderWaveformHalf: use borderWaveformFull
      0x80,  // borderWaveformGray: hold at VCOM; follow-LUT (0x01) drives the border
             // black under the grayscale LUT (black frame on every AA/cover refresh)
      true,  // grayPowerUpFirst: vendor sequences power down after every refresh, so
             // settle the rails before the short gray LUT phases (see Ssd1677Config)
  };
  return cfg;
}

Ssd1677Driver::Ssd1677Driver(const Ssd1677Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight),
      _mirrorX(BoardConfig::ACTIVE.orientation.mirrorX),
#if defined(FREEINK_DISPLAY_FLIPPED) || defined(FLIPPED)
      _mirrorY(true) {}  // FREEINK_DISPLAY_FLIPPED maps to mirrorY
#else
      _mirrorY(BoardConfig::ACTIVE.orientation.mirrorY) {}
#endif

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

  // Driver output control: display height + scan direction. mirrorY flips the
  // gate scan order (TB bit) for an upside-down mount.
  bus.cmd(CMD_DRIVER_OUTPUT_CONTROL);
  bus.data((_h - 1) % 256);
  bus.data((_h - 1) / 256);
  bus.data(_mirrorY ? (_cfg.driverOutputScan | SCAN_TB_FLIP) : _cfg.driverOutputScan);

  bus.cmd(CMD_BORDER_WAVEFORM);
  bus.data(_cfg.borderWaveformInit);

  setRamArea(bus, 0, 0, _w, _h);

  bus.cmd(CMD_AUTO_WRITE_BW_RAM);
  bus.data(0xF7);
  bus.waitBusy(" CMD_AUTO_WRITE_BW_RAM");

  bus.cmd(CMD_AUTO_WRITE_RED_RAM);
  bus.data(0xF7);
  bus.waitBusy(" CMD_AUTO_WRITE_RED_RAM");

  _isScreenOn = false;
  // Override boards can't use _isScreenOn to detect a cold start (their fast
  // sequence powers down after every page), so arm an explicit one-shot full
  // refresh for the first paint — it clears the boot screen and seeds the baseline.
  _needsInitialFull = (_cfg.fullSeqOverride != 0);
}

void Ssd1677Driver::setRamArea(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  // Data-entry bit0 = X direction (1=increment, 0=decrement); bit1 = Y (0=dec).
  // Default is X-increment, Y-decrement. mirrorX reverses the column direction so
  // the controller fills RAM right-to-left, completing a horizontal flip (and,
  // with mirrorY's gate-scan flip, a full 180°). Every RAM write routes through
  // here, so this one branch mirrors the BW, grayscale-strip, and window paths.
  const uint8_t dataEntry = _mirrorX ? 0x00 : 0x01;  // X dec : X inc (Y dec)
  const uint16_t xLo = x, xHi = x + w - 1;
  const uint16_t xStart = _mirrorX ? xHi : xLo;  // counter origin follows X dir
  const uint16_t xEnd = _mirrorX ? xLo : xHi;

  // Gates are physically reversed on this panel.
  y = _h - y - h;

  bus.cmd(CMD_DATA_ENTRY_MODE);
  bus.data(dataEntry);

  bus.cmd(CMD_SET_RAM_X_RANGE);
  bus.data(xStart % 256);
  bus.data(xStart / 256);
  bus.data(xEnd % 256);
  bus.data(xEnd / 256);

  bus.cmd(CMD_SET_RAM_Y_RANGE);
  bus.data((y + h - 1) % 256);
  bus.data((y + h - 1) / 256);
  bus.data(y % 256);
  bus.data(y / 256);

  bus.cmd(CMD_SET_RAM_X_COUNTER);
  bus.data(xStart % 256);
  bus.data(xStart / 256);

  bus.cmd(CMD_SET_RAM_Y_COUNTER);
  bus.data((y + h - 1) % 256);
  bus.data((y + h - 1) / 256);
}

void Ssd1677Driver::writeRam(EpdBus& bus, uint8_t ramCmd, const uint8_t* data, uint32_t size) {
  bus.cmd(ramCmd);
  bus.data(data, static_cast<uint16_t>(size));
}

void Ssd1677Driver::refresh(EpdBus& bus, RefreshMode mode, bool turnOff, bool async) {
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  const uint32_t dbgStart = millis();
  const char* dbgMode = (mode == RefreshMode::Full) ? "FULL" : (mode == RefreshMode::Half) ? "HALF" : "FAST";
#endif
  bus.cmd(CMD_DISPLAY_UPDATE_CTRL1);
  bus.data((mode == RefreshMode::Fast) ? CTRL1_NORMAL : CTRL1_BYPASS_RED);

  // Per-board absolute update sequence (vendor 0x22 values). When set, it selects
  // the panel's waveform directly — including load-temperature and the partial/DU
  // display mode — and self-cycles power. The X4's incremental bit assembly below
  // doesn't trigger some panels' DU waveform (they then run the full waveform on
  // every "fast" refresh); these values fix that. Skipped while a custom grayscale
  // LUT is active (that path needs the 0x0C sequence with the loaded LUT).
  const uint8_t seqOverride = (mode == RefreshMode::Fast) ? _cfg.fastSeqOverride
                              : (mode == RefreshMode::Half && _cfg.halfSeqOverride != 0)
                                  ? _cfg.halfSeqOverride
                                  : _cfg.fullSeqOverride;
  if (seqOverride != 0 && !_customLutActive) {
    // Track the border waveform to the refresh mode (vendor parity): a partial/DU
    // (fast) refresh leaves the border driven dark if it keeps the full-refresh
    // border, producing a black ring around the page. 0 = leave the init value.
    const uint8_t border = (mode == RefreshMode::Fast) ? _cfg.borderWaveformFast
                           : (mode == RefreshMode::Half && _cfg.borderWaveformHalf != 0)
                               ? _cfg.borderWaveformHalf
                               : _cfg.borderWaveformFull;
    if (border != 0) {
      bus.cmd(CMD_BORDER_WAVEFORM);
      bus.data(border);
    }
    if (mode == RefreshMode::Half) {
      bus.cmd(CMD_WRITE_TEMP);
      bus.data(_cfg.halfRefreshTemp);
    }
    bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
    bus.data(seqOverride);
    bus.cmd(CMD_MASTER_ACTIVATION);
    if (!async) bus.waitBusy("refresh");
    // The sequence powered the panel down at the end, but keep the flag truthful
    // to intent: leave it "on" between active updates so display() doesn't force a
    // full HALF refresh next time (which would defeat fast refresh). turnOff marks
    // it off for the sleep path. The vendor sequences self-cycle power: if they
    // include the disable bits (0x03) the panel is OFF afterward — track that so the
    // next refresh (e.g. the custom-LUT grayscale path) powers it back on instead of
    // issuing a display command against a powered-down panel (which hangs BUSY).
    _isScreenOn = (seqOverride & 0x03) ? false : !turnOff;
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
    esp_rom_printf("[SSD1677] %s refresh %ums (ctrl2=0x%x, seq)\n", dbgMode, (unsigned)(millis() - dbgStart),
                   seqOverride);
#endif
    return;
  }

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
  if (!async) bus.waitBusy("refresh");
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  // esp_rom_printf hits the always-on IDF console; Serial (HWCDC) drops on S3.
  esp_rom_printf("[SSD1677] %s refresh %ums (ctrl2=0x%x)\n", dbgMode, (unsigned)(millis() - dbgStart), displayMode);
#endif
}

void Ssd1677Driver::powerOn(EpdBus& bus) {
  if (_isScreenOn) return;
  bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
  bus.data(0xC0);  // CLOCK_ON | ANALOG_ON
  bus.cmd(CMD_MASTER_ACTIVATION);
  bus.waitBusy("gray power-on");
  _isScreenOn = true;
}

void Ssd1677Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayImpl(bus, fb, prev, mode, turnOff, /*async=*/false);
}

// Async: fire the refresh and return; the facade polls BUSY and guards the
// next operation. Skips the single-buffer post-refresh baseline resync — the
// facade supplies `prev` (its shadow of the last-displayed frame) on every
// async update, so RED is rewritten fresh each time instead.
void Ssd1677Driver::displayAsync(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode) {
  displayImpl(bus, fb, prev, mode, /*turnOff=*/false, /*async=*/true);
}

void Ssd1677Driver::displayImpl(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff,
                                bool async) {
  // The first paint after boot/wake must be a FULL refresh: a partial/DU refresh
  // only drives pixels that differ from the RED "old" plane, so it can't clear what
  // is physically on the panel at boot (the black boot screen) — that ghosts through
  // forever otherwise. One full clear also seeds RED with a correct baseline for the
  // fast pages that follow.
  if (!turnOff) {
    if (_needsInitialFull) {
      mode = RefreshMode::Full;
      _needsInitialFull = false;
    } else if (!_isScreenOn && _cfg.fullSeqOverride == 0) {
      // X4-class cold start: panel asleep -> a (warmed) HALF full-clear. Override
      // boards skip this — their fast sequence self-powers, so _isScreenOn is false
      // every page and forcing HALF would make every page a slow full-waveform flash.
      mode = RefreshMode::Half;
    }
  }

  // A grayscale frame leaves the LUT loaded; revert to clean B/W first.
  // grayscaleRevert() runs the revert waveform.
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

  refresh(bus, mode, turnOff, async);

  // Stock X4 syncs both controller RAM planes after activation. Do the same in
  // single-buffer mode so the next differential update starts from a matched
  // BW/RED baseline instead of assuming BW survived the refresh unchanged.
  // (Async updates always come with a facade-owned prev, so this never runs
  // while a refresh is still in flight.)
  if (prev == nullptr && !async) {
    setRamArea(bus, 0, 0, _w, _h);
    writeRam(bus, CMD_WRITE_RAM_BW, fb, _bufferSize);
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
    writeRam(bus, CMD_WRITE_RAM_BW, windowBuffer.data(), windowBufferSize);
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
  const uint16_t len = static_cast<uint16_t>(static_cast<uint32_t>(numRows) * _wb);
  const uint8_t ramCmd = (plane == GrayPlane::Lsb) ? CMD_WRITE_RAM_BW : CMD_WRITE_RAM_RED;
  setRamArea(bus, 0, yStart, _w, numRows);
  bus.cmd(ramCmd);
  bus.data(rows, len);
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
    // Settled rails before the gray waveform (no-op where the panel is already
    // on, i.e. the X4's fast path). refresh() then sends 0x0C, not 0xCC.
    if (_cfg.grayPowerUpFirst) powerOn(bus);
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
  // Same rail-settling rule as displayGray: the revert LUT's phases are just as
  // short, and an under-driven revert leaves gray residue behind.
  if (_cfg.grayPowerUpFirst) powerOn(bus);
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

  // The border register keeps its last value across refreshes; a follow-LUT value
  // (Sticky's 0x01) would now track the loaded grayscale LUT and drive the border
  // black. Park it while the custom LUT is active; the next normal refresh restores
  // the per-mode border via the seqOverride path.
  if (_cfg.borderWaveformGray != 0) {
    bus.cmd(CMD_BORDER_WAVEFORM);
    bus.data(_cfg.borderWaveformGray);
  }

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
// Select the per-board config from the active profile — no extra build flag, it
// follows the -DFREEINK_DEVICE_<NAME> selection (ACTIVE.board). Boards not listed
// use the X4/GDEQ0426T82 defaults.
static const Ssd1677Config& ssd1677ActiveConfig() {
  switch (BoardConfig::ACTIVE.board) {
    case BoardConfig::Board::Sticky: return ssd1677StickyConfig();
    default: return ssd1677DefaultConfig();
  }
}
#endif

PanelDriver& ssd1677Driver() {
  static Ssd1677Driver instance(ssd1677ActiveConfig());
  return instance;
}

}  // namespace freeink
