#include "Ssd1677Driver.h"
#if __has_include(<esp_rom_sys.h>)
#include <esp_rom_sys.h>  // ROM printf reaches UART0 consoles (e.g. a USB-serial bridge) when Serial is native USB
#define EPD_LOG(...) esp_rom_printf(__VA_ARGS__)
#else
#define EPD_LOG(...) Serial.printf(__VA_ARGS__)
#endif

#include <BoardConfig.h>
#if __has_include(<driver/gpio.h>)
#include <driver/gpio.h>  // gpio_hold_dis (Picco/Sticky panel probe)
#endif
#if FREEINK_DEVICE_STICKY
#include <SPI.h>  // hand the shared bus pins back after the Sticky panel probe
#endif

#include <cstring>
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
  // Xteink X4 / GDEQ0426T82 defaults. The stock X4 paths and the X4 Pro 7.4.4
  // SSD1677 firmware use absolute sequences rather than incremental 0x1C:
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
      0xF7,  // fullSeqOverride: stock X4 full update sequence
      0xFC,  // fastSeqOverride: stock X4 partial update sequence
      0xD7,  // halfSeqOverride: stock X4 warmed/full-clean update sequence
      0xC0,  // borderWaveformFull: stock X4 border
      0xC0,  // borderWaveformFast: stock X4 border
      0xC0,  // borderWaveformHalf: stock X4 border
      0xC0,  // borderWaveformGray: written explicitly with the external AA LUT (vendor
             // reference stage 2); same value the B/W paths leave in the register, so
             // the wire state is unchanged — just no longer relying on carry-over
      false, // grayPowerUpFirst
      true,  // absoluteGrayscale: verified X4 factory LUT
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
      lut_grayscale_sticky,  // own copy: voltage tail is per-module, tune there
                             // (see Ssd1677Luts.h), never in the shared X4 LUT
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
      true,  // absoluteGrayscale
  };
  return cfg;
}

#if FREEINK_DEVICE_METALIO_EINK4
// GDEM0397T81 settings from metalio-hw-test. B/W refreshes use its OTP waveforms
// via the vendor 0x22 sequence values; the vendor's separately supplied partial
// LUT is unusable (113 initializers for a 112-byte array), so grayscale/AA uses
// the X4 waveforms instead — same SSD1677 controller and 800x480 geometry, and
// the external-LUT path never touches OTP. Both LUTs are the Metalio forks with
// the per-module voltage tail (VSH1 bumped for darker dark tones — see
// Ssd1677Luts.h). borderWaveformGray matches the X4's 0xC0 written alongside
// that LUT.
static const Ssd1677Config& ssd1677MetalioConfig() {
  static const Ssd1677Config cfg = {
      {0xAE, 0xC7, 0xC3, 0xC0, 0x80}, 0x02, 0x01, 0x6A, lut_grayscale_metalio,
      0xF7, 0xFC, 0xD7, 0x01, 0x80, 0x01, 0xC0, false, true, true, true, true, true,
      lut_factory_quality_metalio, lut_fast_metalio};
  return cfg;
}
#endif

#if FREEINK_DEVICE_PICCO || FREEINK_DEVICE_STICKY
// Stock ID read: reset, then the ID command with SDA released for a half-duplex
// read, bit-banged at ~100 kHz. Reset timing is per board (stock values).
static void epdReadId3Wire(uint8_t cmdByte, uint8_t* id, uint8_t n, uint16_t rstLowMs, uint16_t settleMs) {
  const auto& d = BoardConfig::ACTIVE.display;
  // The deep-sleep path latches the display pins (RESET) with gpio_hold_en; a held
  // pad ignores writes, so after a wake the reset pulse would never reach the panel.
  for (const int8_t pin : {d.cs, d.dc, d.sclk, d.mosi, d.rst, d.busy, d.powerEnable}) {
    if (pin >= 0) gpio_hold_dis(static_cast<gpio_num_t>(pin));
  }
  if (d.powerEnable >= 0 && digitalRead(d.powerEnable) != HIGH) {  // the panel must be powered to answer
    pinMode(d.powerEnable, OUTPUT);
    digitalWrite(d.powerEnable, HIGH);
    delay(50);
  }
  pinMode(d.cs, OUTPUT);
  digitalWrite(d.cs, HIGH);
  pinMode(d.dc, OUTPUT);
  digitalWrite(d.dc, HIGH);
  pinMode(d.sclk, OUTPUT);
  digitalWrite(d.sclk, LOW);
  pinMode(d.mosi, OUTPUT);
  pinMode(d.busy, INPUT_PULLUP);
  pinMode(d.rst, OUTPUT);
  digitalWrite(d.rst, LOW);
  delay(rstLowMs);
  digitalWrite(d.rst, HIGH);
  delay(settleMs);

  const auto clockPulse = [&] {
    digitalWrite(d.sclk, HIGH);
    delayMicroseconds(5);
    digitalWrite(d.sclk, LOW);
    delayMicroseconds(5);
  };
  digitalWrite(d.cs, LOW);
  digitalWrite(d.dc, LOW);
  delayMicroseconds(5);
  for (uint8_t cmd = cmdByte, i = 0; i < 8; i++, cmd <<= 1) {
    digitalWrite(d.mosi, (cmd & 0x80) ? HIGH : LOW);
    clockPulse();
  }
  pinMode(d.mosi, INPUT_PULLUP);
  digitalWrite(d.dc, HIGH);
  delayMicroseconds(5);
  for (uint8_t k = 0; k < n; k++) {
    uint8_t b = 0;
    for (uint8_t i = 0; i < 8; i++) {
      b = static_cast<uint8_t>((b << 1) | (digitalRead(d.mosi) == HIGH ? 1 : 0));
      clockPulse();
    }
    id[k] = b;
  }
  digitalWrite(d.cs, HIGH);
  pinMode(d.mosi, OUTPUT);
}
#endif

#if FREEINK_DEVICE_STICKY
// Newer Sticky units carry an SSD2677 (UltraChip-style command set, see
// Ssd2677Driver) in place of the SSD1677. Stock v1.2.0 tells them apart by CMD
// 0x70: an SSD2677 answers 0x07 first (read half-duplex on SDA after a 20 ms /
// 10 ms reset); anything else is the SSD1677. The BUSY line also differs once
// the controller is idle after reset: SSD1677 BUSY is active-high (idle low),
// SSD2677 BUSY active-low (idle high), so an idle-high BUSY also selects the
// SSD2677 when the 0x70 reply is not conclusive. Cached. The display shares its
// SPI bus with the SD card, so a bus SPI already runs is restarted afterwards.
bool stickyHasSsd2677() {
  static int8_t cached = -1;
  if (cached >= 0) return cached == 1;
  uint8_t rev = 0;
  epdReadId3Wire(0x70, &rev, 1, 20, 10);
  delay(30);  // let either controller finish its post-reset busy phase
  const bool busyHigh = digitalRead(BoardConfig::ACTIVE.display.busy) == HIGH;
  cached = (rev == 0x07 || busyHigh) ? 1 : 0;
  // Turning the shared pins into GPIOs detached them from the SPI bus the SD card
  // already started (the pin manager may stop the bus). Restart it on the same
  // pins so both the SD card and the display driver find it working.
  if (SPI.bus()) {
    const auto& b = BoardConfig::ACTIVE;
    const int8_t ss = SPI.pinSS();
    SPI.end();
    SPI.begin(b.display.sclk, b.sd.miso, b.display.mosi, ss);  // the shared bus: display SCLK/MOSI, SD MISO
  }

  // Logged unconditionally: a wrong pick stalls in BUSY waits, so this line
  // must get out first.
  EPD_LOG("[%u] [EPD] Sticky CMD 0x70 reply %02X, BUSY idle %s: %s\n", static_cast<unsigned>(millis()), rev,
          busyHigh ? "high" : "low", cached ? "SSD2677" : "SSD1677");
  return cached == 1;
}
#endif

#if FREEINK_DEVICE_PICCO
// Onyx Picco / BOOX Tiles. Retail units carry one of three SSD1677 panels, told
// apart (as the stock firmware does) by the 10-byte user ID from CMD 0x2E:
//   YRD0426   CA FE 00 16 77 02 10 01 00 91, or 00 AF 00 01 AA 09 12 20 69 00
//   DEPG0397  any other ID starting CA FE 00 16 77
//   OPM040B3  "W040B3"
// None of those -> the stock SSD2677 path, SE0400NQW47 (see Ssd2677Driver).
// Per-panel values are the stock tables (build 2912): YRD0426 and OPM040B3 run
// FULL/HALF on OTP (0xF7/0xD7) with a custom FAST LUT; DEPG0397 uses custom LUTs
// for every mode (its FAST is stock's mode-2 table, which cut boot ghosting). Border at init: 0x01 (YRD0426, DEPG0397), 0x03 (OPM040B3).
// Grayscale follows the Sticky (see piccoConfig).

static uint8_t piccoClassify(const uint8_t id[10]) {
  static const uint8_t yrdA[10] = {0xCA, 0xFE, 0x00, 0x16, 0x77, 0x02, 0x10, 0x01, 0x00, 0x91};
  static const uint8_t yrdB[10] = {0x00, 0xAF, 0x00, 0x01, 0xAA, 0x09, 0x12, 0x20, 0x69, 0x00};
  static const uint8_t opm[10] = {'W', '0', '4', '0', 'B', '3', 0, 0, 0, 0};
  if (memcmp(id, yrdA, 10) == 0 || memcmp(id, yrdB, 10) == 0) return PiccoYrd0426;
  if (memcmp(id, yrdA, 5) == 0) return PiccoDepg0397;
  if (memcmp(id, opm, 10) == 0) return PiccoOpm040b3;
  return PiccoUnknown;
}

static Ssd1677Config piccoConfig(uint8_t border, const unsigned char* fast, const unsigned char* full) {
  Ssd1677Config c = {{0xAE, 0xC7, 0xC3, 0xC0, 0x80}, DRIVER_OUTPUT_SCAN, border, 0x5A, lut_grayscale_sticky,
                     0xF7, 0xDC, 0xD7};
  // AA grayscale is the Sticky's overlay path (same 3.97" glass class): its LUT
  // and voltages, power up first, 0xCC, border held at VCOM. Stock's own slot-5
  // waveform (CTRL2 0xC4, B/W voltages left loaded) gave poor AA on DEPG0397,
  // while this path looked right on hardware. Absolute (images, sleep screens)
  // uses the SDK's X4 factory LUT, also as on the Sticky.
  c.grayPowerUpFirst = true;
  c.borderWaveformGray = 0x80;
  c.overlayGrayscale = true;
  c.absoluteGrayscale = true;
  c.fastLut = fast;
  c.fullLut = full;
  c.halfLut = full;  // DEPG0397 slot 0 (FULL) and slot 3 (HALF) are the same table
  return c;
}

uint8_t piccoProbePanel() {
  static bool probed = false;
  if (probed) return BoardConfig::ACTIVE.displayControllerVariant;
  probed = true;
  uint8_t id[10] = {};
  epdReadId3Wire(0x2E, id, 10, 10, 40);
  uint8_t panel = piccoClassify(id);
  static const char* const names[] = {"unknown", "YRD0426", "DEPG0397", "OPM040B3", "SE0400NQW47"};
  if (panel == PiccoUnknown) {
    // Stock falls back to its SSD2677 driver, which identifies SE0400NQW47 by
    // the 3-byte CMD 0x70 reply 07 01 01.
    uint8_t rev[3] = {};
    epdReadId3Wire(0x70, rev, 3, 10, 40);
    if (rev[0] == 0x07 && rev[1] == 0x01 && rev[2] == 0x01) panel = PiccoSe0400nqw47;
    if (Serial) Serial.printf("[%lu] [EPD] Picco CMD 0x70 reply %02X %02X %02X\n", millis(), rev[0], rev[1], rev[2]);
  }
  BoardConfig::ACTIVE.displayControllerVariant = panel;
  if (Serial) {
    Serial.printf("[%lu] [EPD] Picco panel %s, ID %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", millis(),
                  names[panel], id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9]);
  }
  return panel;
}

static const Ssd1677Config& ssd1677PiccoConfig() {
  static const Ssd1677Config yrd = piccoConfig(0x01, lut_picco_fast, nullptr);
  static const Ssd1677Config depg = piccoConfig(0x01, lut_picco_depg_fast, lut_picco_depg_full);
  static const Ssd1677Config opm = piccoConfig(0x03, lut_picco_fast, nullptr);

  switch (piccoProbePanel()) {
    case PiccoYrd0426: return yrd;
    case PiccoDepg0397: return depg;
    case PiccoOpm040b3: return opm;
    default: return ssd1677StickyConfig();  // unrecognized ID: previous generic config
  }
}
#endif

// ── Reusable per-board waveform shortcuts ────────────────────────────────────
// Opt-in optimizations a board can layer onto a base Ssd1677Config when its
// specific panel is known to tolerate them. Each is a pure copy-and-tweak so a
// board picks only the shortcuts it has validated — apply like:
//   static const Ssd1677Config cfg = fastDuRefreshShortcut(ssd1677DefaultConfig());
// and add a `case Board::Xxx:` in ssd1677ActiveConfig() returning it.

// Fast-DU refresh shortcut (~77 ms/refresh). A fast BW refresh uses the
// incremental CTRL2=0x1C differential-update path (load LUT + display) instead
// of a 0xFC-style vendor sequence that also reloads temperature and re-cycles
// clock/analog on every refresh. SAFE ONLY on panels that honor 0x1C as a true
// partial update; on panels where 0x1C silently promotes to the FULL waveform
// this is a big regression, so leave fastSeqOverride != 0 there. Validated on
// the Xteink X4 (the CrossPoint community-sdk drives that exact panel with 0x1C).
static Ssd1677Config fastDuRefreshShortcut(Ssd1677Config base) {
  base.fastSeqOverride = 0;  // 0 -> incremental CTRL2=0x1C DU path
  return base;
}

// Xteink X4 with the fast-DU shortcut — OPT-IN via -DFREEINK_X4_FAST_DU_SHORTCUT.
// The X4 default stays the stock 0xFC absolute partial sequence: the 0x1C path
// skips the per-refresh temperature load and power sequencing, which is the
// community-sdk behavior the stock-parity work moved away from after ghosting /
// blotching reports on some panels ("weaker partial selection shows heavy
// ghosting", see ssd1677DefaultConfig). Enable the flag only after validating
// long reading sessions across temperatures on your panel (~85 ms/refresh win).
// A d2-class board that shares this panel can reuse ssd1677X4Config() or build
// its own base and layer the same shortcut(s).
#ifdef FREEINK_X4_FAST_DU_SHORTCUT
static const Ssd1677Config& ssd1677X4Config() {
  static const Ssd1677Config cfg = fastDuRefreshShortcut(ssd1677DefaultConfig());
  return cfg;
}
#endif

// Xteink X4 Pro with the fast-DU shortcut — OPT-IN via
// -DFREEINK_X4PRO_FAST_DU_SHORTCUT. The X4 Pro paints on the stock X4 config
// (same GDEQ0426T82 panel class — see ssd1677ActiveConfig), so the same
// ~85 ms/refresh win applies, and so does the same panel-variance caveat: 0x1C
// skips the per-refresh temperature load and power sequencing, and artifacts
// tend to appear only over long sessions and across temperature. Enable only
// after validating on your unit. SSD1677-batch units only — UC8179/UC8279
// batches select a different driver and never reach this config.
#ifdef FREEINK_X4PRO_FAST_DU_SHORTCUT
static const Ssd1677Config& ssd1677X4ProConfig() {
  static const Ssd1677Config cfg = fastDuRefreshShortcut(ssd1677DefaultConfig());
  return cfg;
}
#endif

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
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 20000000;
}

PanelGeometry Ssd1677Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

void Ssd1677Driver::begin(EpdBus& bus) {
  bus.reset();
  initController(bus);
}

void Ssd1677Driver::initController(EpdBus& bus) {
  constexpr uint8_t TEMP_SENSOR_INTERNAL = 0x80;

  bus.cmd(CMD_SOFT_RESET);
  // The X4 Pro production sequence requires a fixed 10 ms settle after
  // SWRESET. Active-high BUSY may not have asserted by the first GPIO sample,
  // so waitBusy() alone is not a substitute for this delay.
  delay(10);
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
  _pendingPowerOff = false;
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  const uint32_t dbgStart = millis();
  const char* dbgMode = (mode == RefreshMode::Full) ? "FULL" : (mode == RefreshMode::Half) ? "HALF" : "FAST";
#endif
  bus.cmd(CMD_DISPLAY_UPDATE_CTRL1);
  bus.data((mode == RefreshMode::Fast) ? CTRL1_NORMAL : CTRL1_BYPASS_RED);
  if (_cfg.writeSecondUpdateControlByte) bus.data(0x00);
  if (_cfg.restoreInternalTemperature && mode != RefreshMode::Half) {
    bus.cmd(CMD_TEMP_SENSOR_CONTROL);
    bus.data(0x80);
  }
  // Board B/W LUT for this mode: load it so the custom-LUT branch below activates
  // it without the OTP reload. Not while a grayscale LUT is already loaded.
  const unsigned char* modeLut = (mode == RefreshMode::Fast)   ? _cfg.fastLut
                                 : (mode == RefreshMode::Half) ? _cfg.halfLut
                                                               : _cfg.fullLut;
  const bool boardLut = modeLut != nullptr && !_customLutActive;
  if (boardLut) {
    setCustomLut(bus, true, modeLut);
    if (mode == RefreshMode::Fast && _cfg.borderWaveformFast != 0) {
      bus.cmd(CMD_BORDER_WAVEFORM);
      bus.data(_cfg.borderWaveformFast);
    }
  }

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
    if (!async) bus.waitRefreshComplete("refresh");
    // Only sequences carrying the low two disable bits physically power down.
    // X4 Pro DU (0xFC) does not, so a turnOff request must run the documented
    // 0x3C=0x80, 0x22=0x03, 0x20 sequence after the waveform completes instead
    // of merely changing the software flag.
    const bool sequencePowersOff = (seqOverride & 0x03) != 0;
    _isScreenOn = !sequencePowersOff;
    if (turnOff && !sequencePowersOff) {
      // Defer until displayImpl/displayWindow has completed any post-refresh RAM
      // rewire. Async updates consume the same flag in displayFinish().
      _pendingPowerOff = true;
    }
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
    esp_rom_printf("[SSD1677] %s refresh %ums (ctrl2=0x%x, seq)\n", dbgMode, (unsigned)(millis() - dbgStart),
                   seqOverride);
#endif
    return;
  }

  if (boardLut && mode != RefreshMode::Fast) {
    // Board FULL/HALF LUT: clock/analog on + display mode 1 without the OTP LUT
    // load (0x10 clear); FULL loads the temperature (0xE4), HALF writes it (0xC4).
    uint8_t ctrl2 = 0xE4;
    if (mode == RefreshMode::Half) {
      bus.cmd(CMD_WRITE_TEMP);
      bus.data(_cfg.halfRefreshTemp);
      ctrl2 = 0xC4;
    }
    if (turnOff) ctrl2 |= 0x03;
    _isScreenOn = !turnOff;
    bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
    bus.data(ctrl2);
    bus.cmd(CMD_MASTER_ACTIVATION);
    if (!async) bus.waitRefreshComplete("refresh");
    setCustomLut(bus, false, nullptr);
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
  } else if (_customLutActive) {
    // External-LUT (AA grayscale) activation is the absolute 0xCC sequence per the
    // vendor reference — clock/analog enable + display, WITHOUT the OTP LUT reload
    // (0x10 bit clear). The enable bits are a no-op when the rails are already up
    // (the usual X4 case, where stage 1 left them on), and required when they are
    // not, so 0xCC is correct in both states. The OEM AA path leaves power
    // enabled: only the low disable bits added for turnOff power it down.
    displayMode = 0xCC;
    if (turnOff) displayMode |= 0x03;
    _isScreenOn = !turnOff;
  } else {  // Fast
    displayMode |= 0x1C;
  }

  bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
  bus.data(displayMode);
  bus.cmd(CMD_MASTER_ACTIVATION);
  if (!async) bus.waitRefreshComplete("refresh");
  if (boardLut) setCustomLut(bus, false, nullptr);
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

void Ssd1677Driver::powerOffBetweenFrames(EpdBus& bus) {
  // Picco idle shutdown uses BUSY, with one tick for command propagation.
  // Other callers keep their existing production shutdown timing.
  powerOffController(bus, 1);
}

void Ssd1677Driver::powerOffController(EpdBus& bus, uint16_t settleMs) {
  if (!_isScreenOn) return;
  bus.cmd(CMD_BORDER_WAVEFORM);
  bus.data(_cfg.borderWaveformInit);  // X4 Pro: 0x80
  bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
  bus.data(0x03);  // ANALOG_OFF_PHASE | CLOCK_OFF
  bus.cmd(CMD_MASTER_ACTIVATION);
  // Production X4 Pro power-off time. If BUSY remains asserted after the fixed
  // interval, wait out the remainder before changing the state flag.
  delay(settleMs);
  bus.waitBusy(" display power-down");
  _isScreenOn = false;
}

void Ssd1677Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayImpl(bus, fb, prev, mode, turnOff, /*async=*/false);
}

// Deferred refresh: fire the update and return; displayFinish() waits it out.
// Skips the single-buffer post-refresh baseline resync — the facade supplies
// `prev` (its shadow) on shadowed updates, and the no-shadow/grayscale flow
// re-seeds the baseline itself (cleanupGrayscaleBuffers).
bool Ssd1677Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode,
                                 bool turnOff) {
  displayImpl(bus, fb, prev, mode, turnOff, /*async=*/true);
  return true;
}

void Ssd1677Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  bus.waitRefreshComplete("refresh");
  if (_pendingFrameSync && fb) {
    // The facade retains the displayed frame until finish (or supplies its
    // shadow). Never rewrite the differential baseline during the waveform.
    setRamArea(bus, 0, 0, _w, _h);
    writeRam(bus, CMD_WRITE_RAM_BW, fb, _bufferSize);
    writeRam(bus, CMD_WRITE_RAM_RED, fb, _bufferSize);
  }
  _pendingFrameSync = false;
  if (_pendingPowerOff) {
    _pendingPowerOff = false;
    powerOffController(bus);
  }
}

void Ssd1677Driver::displayImpl(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff,
                                bool async) {
  const bool cold = _needsInitialFull;
  _pendingFrameSync = _cfg.blackPulseClean && async;
  if (_needsGrayClear) {
    if (mode == RefreshMode::Fast) mode = _cfg.halfSeqOverride ? RefreshMode::Half : RefreshMode::Full;
    _needsGrayClear = false;
  }
  // The first paint after boot/wake must be an absolute clean, not a partial/DU
  // refresh: a partial only drives pixels that differ from the RED "old" plane, so
  // it can't clear what is physically on the panel at boot (e.g. the sleep screen).
  // Stock parity: the OEM firmware's only clean primitive in normal operation is
  // the single-pass HALF (0xD7 "_updateFull"); it never runs the multi-flash OTP
  // full waveform (0xF7 is a dead fallback branch there). Promote to HALF where
  // the board has one; boards without (Sticky) promote to their vendor FULL.
  if (_needsInitialFull) {
    // First paint after boot/wake must not be a differential FAST: it only drives
    // pixels that differ from the RED baseline, so it can't clear whatever is
    // physically on the panel (the boot screen) and that ghosts through. But a HALF
    // or FULL the caller already asked for is non-differential (BYPASS_RED) and
    // clears the panel + seeds the baseline on its own — honor it and just consume
    // the one-shot. Only upgrade a FAST request. This keeps the boot logo (a HALF)
    // from paying an extra multi-inversion FULL-waveform flash on top of its own.
    //
    // Runs for turnOff requests too. The waveform a paint needs is independent of
    // whether the rails drop afterwards, and a caller that powers down on every
    // refresh (CrossPoint's sunlight fading fix) would otherwise never reach this
    // one-shot at all, leaving the sleep screen on the glass after every wake.
    if (mode == RefreshMode::Fast) {
      mode = (_cfg.halfSeqOverride != 0) ? RefreshMode::Half : RefreshMode::Full;
    }
    _needsInitialFull = false;
  } else if (!turnOff && !_isScreenOn && _cfg.fullSeqOverride == 0) {
    // X4-class cold start: panel asleep -> a (warmed) HALF full-clear. Override
    // boards skip this — their fast sequence self-powers, so _isScreenOn is false
    // every page and forcing HALF would make every page a slow full-waveform flash.
    // A turnOff caller powers the panel down itself after every refresh, so
    // !_isScreenOn carries no cold-start signal there either; leave it gated.
    mode = RefreshMode::Half;
  }

  // Leaving grayscale content without the firmware's cleanup: stock parity — the
  // OEM firmware has NO revert waveform (its grayscale sequence just resyncs RED
  // afterwards). RED still holds the gray MSB plane here, so it can't serve as a
  // differential baseline; promote a Fast update to the single-pass HALF clean
  // (absolute waveform, both planes rewritten below), which is also how stock
  // erases AA residue (its periodic 0xD7 promote).
  if (_inGrayscaleMode) {
    _inGrayscaleMode = false;
    if (mode == RefreshMode::Fast) {
      mode = RefreshMode::Half;
    }
  }

  setRamArea(bus, 0, 0, _w, _h);

  if (_cfg.blackPulseClean && mode == RefreshMode::Half) {
    // metalio-hw-test lv_adapter_display.cc::PeriodicBlackPulseClear: avoid
    // the multi-flash OTP D7 waveform, using old -> black -> target via FC.
    // Cold controller RAM has no valid previous image; the demo seeds white.
    if (cold) bus.fillPlane(CMD_WRITE_RAM_RED, 0xFF, _h, _wb);
    else if (prev) writeRam(bus, CMD_WRITE_RAM_RED, prev, _bufferSize);
    bus.fillPlane(CMD_WRITE_RAM_BW, 0x00, _h, _wb);
    refresh(bus, RefreshMode::Fast, false, false);
    setRamArea(bus, 0, 0, _w, _h);
    bus.fillPlane(CMD_WRITE_RAM_BW, 0x00, _h, _wb);
    bus.fillPlane(CMD_WRITE_RAM_RED, 0x00, _h, _wb);
    writeRam(bus, CMD_WRITE_RAM_BW, fb, _bufferSize);
    // Only the final phase may defer completion or power down.
    refresh(bus, RefreshMode::Fast, turnOff, async);
  } else {
    if (mode != RefreshMode::Fast) {
      writeRam(bus, CMD_WRITE_RAM_BW, fb, _bufferSize);
      writeRam(bus, CMD_WRITE_RAM_RED, fb, _bufferSize);
    } else {
      writeRam(bus, CMD_WRITE_RAM_BW, fb, _bufferSize);
      if (prev != nullptr) {
        // Dual-buffer: RED holds the previous frame for the differential compare.
        // Single-buffer (prev == nullptr): RED already holds it from last refresh.
        writeRam(bus, CMD_WRITE_RAM_RED, prev, _bufferSize);
      }
    }

    refresh(bus, mode, turnOff, async);
  }

  // Stock X4 syncs both controller RAM planes after activation. Do the same in
  // single-buffer mode so the next differential update starts from a matched
  // BW/RED baseline instead of assuming BW survived the refresh unchanged.
  // (Async updates always come with a facade-owned prev, so this never runs
  // while a refresh is still in flight.)
  if ((prev == nullptr || _cfg.blackPulseClean) && !async) {
    setRamArea(bus, 0, 0, _w, _h);
    writeRam(bus, CMD_WRITE_RAM_BW, fb, _bufferSize);
    writeRam(bus, CMD_WRITE_RAM_RED, fb, _bufferSize);
  }
  if (!async && _pendingPowerOff) {
    _pendingPowerOff = false;
    powerOffController(bus);
  }
}

void Ssd1677Driver::displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y,
                                  uint16_t w, uint16_t h, bool turnOff) {
  if (x + w > _w || y + h > _h) return;
  if (x % 8 != 0 || w % 8 != 0) return;  // window must be byte-aligned
  if (!fb) return;

  // A windowed diff can't run against grayscale plane contents (RED holds the
  // gray MSB plane, not the previous frame). With no revert waveform (stock
  // parity), exit grayscale by painting the whole frame with the single-pass
  // HALF clean instead; the window content is part of fb, so nothing is lost.
  if (_inGrayscaleMode || _needsGrayClear) {
    displayImpl(bus, fb, nullptr, _cfg.halfSeqOverride ? RefreshMode::Half : RefreshMode::Full, turnOff, /*async=*/false);
    return;
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
  if (_pendingPowerOff) {
    _pendingPowerOff = false;
    powerOffController(bus);
  }
}

void Ssd1677Driver::seedPreviousFrame(EpdBus& bus, const uint8_t* buf) {
  if (!buf) return;
  // Write the frame into RED (the differential "old frame" plane) with no refresh —
  // identical to the RED write display() does for `prev`, so the next prev==nullptr
  // fast refresh diffs the new frame against this baseline instead of a stale one.
  setRamArea(bus, 0, 0, _w, _h);
  writeRam(bus, CMD_WRITE_RAM_RED, buf, _bufferSize);
}

void Ssd1677Driver::beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback, bool turnOff) {
  _absoluteInput = mode == GrayscaleMode::Absolute;
  // Absolute planes supply every target pixel; activate only the final gray image.
  if (!_absoluteInput) displayGrayscaleBase(bus, fb, fallback, turnOff);
}

void Ssd1677Driver::writeGrayRam(EpdBus& bus, uint8_t command, const uint8_t* data, uint16_t len) {
  if (!_absoluteInput) {
    writeRam(bus, command, data, len);
    return;
  }
  // The X4 factory bank selects black at 11 and white at 00. The public
  // absolute encoding is black=00/white=11, so complement each host plane.
  uint8_t chunk[128];
  bus.cmd(command);
  auto txn = bus.beginTxn();
  for (uint32_t offset = 0; offset < len; offset += sizeof(chunk)) {
    const uint16_t count = (len - offset < sizeof(chunk)) ? len - offset : sizeof(chunk);
    for (uint16_t i = 0; i < count; ++i) chunk[i] = static_cast<uint8_t>(~data[offset + i]);
    txn.writeBytes(chunk, count);
  }
}

void Ssd1677Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  if (!lsb) return;
  setRamArea(bus, 0, 0, _w, _h);
  writeGrayRam(bus, CMD_WRITE_RAM_BW, lsb, _bufferSize);
}

void Ssd1677Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (!msb) return;
  setRamArea(bus, 0, 0, _w, _h);
  writeGrayRam(bus, CMD_WRITE_RAM_RED, msb, _bufferSize);
}

void Ssd1677Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                             uint16_t numRows) {
  if (!rows || numRows == 0) return;
  const uint16_t len = static_cast<uint16_t>(static_cast<uint32_t>(numRows) * _wb);
  const uint8_t ramCmd = (plane == GrayPlane::Lsb) ? CMD_WRITE_RAM_BW : CMD_WRITE_RAM_RED;
  setRamArea(bus, 0, yStart, _w, numRows);
  writeGrayRam(bus, ramCmd, rows, len);
}

void Ssd1677Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  if (!_cfg.overlayGrayscale && !_cfg.absoluteGrayscale && lut == nullptr) {
    // Legacy callers may bypass capability negotiation; fall back to B/W.
    display(bus, fb, nullptr, RefreshMode::Full, turnOff);
    return;
  }

  // Differential mode marks grayscale content on the panel (the next BW update
  // must not diff against the gray planes); factory absolute mode self-cleans.
  _inGrayscaleMode = !factoryMode;

  const unsigned char* selectedLut = lut;
  if (selectedLut == nullptr) {
    selectedLut = factoryMode ? (_cfg.factoryGrayLut ? _cfg.factoryGrayLut : lut_factory_quality)
                              : _cfg.grayLut;
  }
  setCustomLut(bus, true, selectedLut);

  if (factoryMode) {
    // Keep shutdown separate from the absolute grayscale activation: combining
    // ANALOG_OFF/CLOCK_OFF with the paint can leave incomplete or smeared grays.
    // Reset CTRL1 to normal — a prior HALF leaves BYPASS_RED set, which would
    // ignore RED RAM and break 4-level grayscale.
    bus.cmd(CMD_DISPLAY_UPDATE_CTRL1);
    bus.data(CTRL1_NORMAL);
    bus.cmd(CMD_DISPLAY_UPDATE_CTRL2);
    bus.data(0xCC);  // CLOCK_ON|ANALOG_ON|MODE_SELECT|DISPLAY_START
    bus.cmd(CMD_MASTER_ACTIVATION);
    bus.waitRefreshComplete("factory_gray");
    _isScreenOn = true;
    if (turnOff) powerOffController(bus);
    _needsGrayClear = true;  // restoring RAM alone cannot restore B/W ink
  } else {
    // Settled rails before the gray waveform (no-op where the panel is already
    // on, i.e. the X4's fast path). refresh() then runs the 0xCC external-LUT
    // sequence (its enable bits are a no-op on already-up rails).
    if (_cfg.grayPowerUpFirst) powerOn(bus);
    refresh(bus, RefreshMode::Fast, turnOff);
  }

  setCustomLut(bus, false, nullptr);
  _absoluteInput = false;
}

void Ssd1677Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw) return;
  setRamArea(bus, 0, 0, _w, _h);
  writeRam(bus, CMD_WRITE_RAM_RED, bw, _bufferSize);
  // The restored BW frame in RED RAM *is* the clean differential baseline for the
  // next BW page turn, so nothing else is needed — clear the flag (stock parity:
  // the OEM firmware's grayscale sequence ends exactly this way, RED resync only;
  // it has no revert waveform at all).
  _inGrayscaleMode = false;
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
  // Stock parity (_powerOff): park the border at its init value so it is not left
  // driven with the full-refresh waveform through deep sleep, then power down
  // analog/clock. Stock does not touch CTRL1 here.
  powerOffController(bus);
  // Stock parity: deep sleep mode 2 (0x03) discards controller RAM. Nothing may
  // treat RAM as a valid diff baseline after wake — initController() re-arms
  // _needsInitialFull, so the first paint is an absolute clean anyway.
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0x03);
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
#if FREEINK_DEVICE_METALIO_EINK4
    case BoardConfig::Board::MetalioEInk4: return ssd1677MetalioConfig();
#endif
    case BoardConfig::Board::Sticky: return ssd1677StickyConfig();
    // Waveshare ESP32-S3-ePaper-3.97: the vendor driver's bring-up is byte-identical
    // to Seeed's (booster AE C7 C3 C0 80, border 0x01, 0x22 = F7 full / FF partial /
    // D7 fast), so it runs the same config. Grayscale LUT is Sticky's — same panel
    // class, but tune here if a unit shows banding.
    case BoardConfig::Board::WsEpaper397: return ssd1677StickyConfig();
#if FREEINK_DEVICE_PICCO
    // Onyx Picco / BOOX Tiles: per-panel config picked from the panel's user ID
    // (see ssd1677PiccoConfig).
    case BoardConfig::Board::Picco: return ssd1677PiccoConfig();
#endif
    // X4 Pro runs on the stock X4/GDEQ0426T82 config — same controller and panel
    // class, confirmed painting on hardware. No custom LUT or drive voltages needed.
    // Layers the fast-DU shortcut only when the build opts in (ssd1677X4ProConfig).
#ifdef FREEINK_X4PRO_FAST_DU_SHORTCUT
    case BoardConfig::Board::XteinkX4Pro: return ssd1677X4ProConfig();
#else
    case BoardConfig::Board::XteinkX4Pro: return ssd1677DefaultConfig();
#endif
    // X4 layers the fast-DU shortcut on the default only when the build has
    // opted in (see ssd1677X4Config); stock 0xFC parity otherwise.
#ifdef FREEINK_X4_FAST_DU_SHORTCUT
    case BoardConfig::Board::XteinkX4: return ssd1677X4Config();
#endif
    default: return ssd1677DefaultConfig();
  }
}
#endif

PanelDriver& ssd1677Driver() {
  static Ssd1677Driver instance(ssd1677ActiveConfig());
  return instance;
}

}  // namespace freeink
