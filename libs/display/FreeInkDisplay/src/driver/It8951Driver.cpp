#include "It8951Driver.h"

#include <BoardConfig.h>

// Compiled only into the M5Paper (classic ESP32) build; the body uses VSPI and
// other classic-ESP32-only symbols, so other MCU builds skip it entirely.
#if FREEINK_DRIVER_IT8951

#include <cstring>

#include "esp_heap_caps.h"

namespace freeink {
namespace {
// SPI preamble words (sent MSB-first before each command/data/read).
constexpr uint16_t PRE_CMD = 0x6000;  // command write
constexpr uint16_t PRE_WR = 0x0000;   // data write
constexpr uint16_t PRE_RD = 0x1000;   // data read

// IT8951 system / I80 commands.
constexpr uint16_t CMD_SYS_RUN = 0x0001;
constexpr uint16_t CMD_STANDBY = 0x0002;
constexpr uint16_t CMD_SLEEP = 0x0003;
constexpr uint16_t CMD_REG_RD = 0x0010;
constexpr uint16_t CMD_REG_WR = 0x0011;
constexpr uint16_t CMD_LD_IMG_AREA = 0x0021;
constexpr uint16_t CMD_LD_IMG_END = 0x0022;
constexpr uint16_t CMD_DEV_INFO = 0x0302;
constexpr uint16_t CMD_DPY_AREA = 0x0034;
constexpr uint16_t CMD_VCOM = 0x0039;

// Registers.
constexpr uint16_t REG_I80CPCR = 0x0004;  // host packed-write enable
constexpr uint16_t REG_LISAR = 0x0208;    // load-image start address (low word; +2 = high word)
constexpr uint16_t REG_LUTAFSR = 0x1224;  // LUT engine busy (0 = idle)

// LD_IMG arg fields: (endian << 8) | (bpp << 4) | rotation.
constexpr uint16_t BPP_4 = 0x02;       // 4 bits/pixel (native 16-gray)
constexpr uint16_t ENDIAN_BIG = 0x01;  // big-endian pixel words

constexpr unsigned long READY_TIMEOUT_MS = 3000;

// One pixel's 4bpp value from CrossPoint's anti-aliasing planes (base B/W + LSB +
// MSB), matching the LilyGo ED047TC1 mapping: a set base bit is white; otherwise
// MSB/LSB pick light/dark; clear is black. IT8951 4bpp: 0x0=black .. 0xF=white.
inline uint8_t gray4(uint8_t base, uint8_t lsb, uint8_t msb, uint8_t mask) {
  if (base & mask) return 0xF;  // white
  const bool l = lsb & mask, m = msb & mask;
  if (m && l) return 0x5;  // dark
  if (m) return 0xA;       // light
  if (l) return 0x5;       // dark
  return 0x0;              // black
}
}  // namespace

const It8951Config& it8951DefaultConfig() {
  static const It8951Config cfg = {
      13,                   // miso (M5Paper: GPIO13, shared with SD)
      10000000,             // 10 MHz SPI
      IT8951_ROTATE_AUTO,   // pick 0°/90° from the reported panel orientation
      0,                    // vcomMv: 0 -> keep the panel's factory OTP VCOM
      2,                    // fullMode  = GC16 (full 16-gray, ghost-clearing)
      3,                    // halfMode  = GL16
      1,                    // fastMode  = DU   (2-level B/W, fast)
      6,                    // grayMode  = DU4  (4-level DIRECT update: differential,
                            //                   only changed pixels move -> no flash)
      0x001236E0,           // imgBufFallbackAddr (typical M5Paper IT8951 buffer base)
  };
  return cfg;
}

It8951Driver::It8951Driver(const It8951Config& cfg)
    : _cfg(cfg),
      _spi(SPI),  // share the Arduino global bus with the SD card (see header)
      _fbW(BoardConfig::ACTIVE.displayWidth),
      _fbH(BoardConfig::ACTIVE.displayHeight),
      _fbWb(BoardConfig::ACTIVE.displayWidth / 8) {}

PanelGeometry It8951Driver::geometry() const {
  return {_fbW, _fbH, _fbWb, static_cast<uint32_t>(_fbWb) * _fbH};
}

void It8951Driver::waitReady() {
  if (_busy < 0) return;
  const unsigned long start = millis();
  while (digitalRead(_busy) == LOW) {
    if (millis() - start > READY_TIMEOUT_MS) {  // fail open rather than hang
#ifdef IT8951_PROBE_DEBUG
      if (Serial) Serial.printf("[it8951] waitReady TIMEOUT (busy pin %d stuck LOW)\n", _busy);
#endif
      return;
    }
  }
}

void It8951Driver::writeWord(uint16_t preamble, uint16_t value) {
  waitReady();
  _spi.beginTransaction(SPISettings(_cfg.spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer16(preamble);
  _spi.transfer16(value);
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
}

void It8951Driver::writeCommand(uint16_t cmd) { writeWord(PRE_CMD, cmd); }
void It8951Driver::writeData(uint16_t data) { writeWord(PRE_WR, data); }

uint16_t It8951Driver::readData() {
  waitReady();
  _spi.beginTransaction(SPISettings(_cfg.spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer16(PRE_RD);
  _spi.transfer16(0x0000);  // dummy word the controller requires before valid data
  const uint16_t v = _spi.transfer16(0x0000);
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
  return v;
}

void It8951Driver::readWords(uint16_t* buf, uint16_t count) {
  waitReady();
  _spi.beginTransaction(SPISettings(_cfg.spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer16(PRE_RD);
  _spi.transfer16(0x0000);  // dummy
  for (uint16_t i = 0; i < count; i++) buf[i] = _spi.transfer16(0x0000);
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
}

void It8951Driver::writeReg(uint16_t reg, uint16_t value) {
  writeCommand(CMD_REG_WR);
  writeData(reg);
  writeData(value);
}

uint16_t It8951Driver::readReg(uint16_t reg) {
  writeCommand(CMD_REG_RD);
  writeData(reg);
  return readData();
}

void It8951Driver::systemRun() { writeCommand(CMD_SYS_RUN); }

void It8951Driver::getDeviceInfo() {
  writeCommand(CMD_DEV_INFO);
  uint16_t info[20];  // PanelW, PanelH, bufAddrL, bufAddrH, FW[8], LUT[8]
  readWords(info, 20);
  _panelW = info[0];
  _panelH = info[1];
  _imgBufAddr = (static_cast<uint32_t>(info[3]) << 16) | info[2];
  // Reject an implausible read (no MISO / cold controller) and fall back so the
  // load path still targets a valid buffer.
  if (_panelW == 0 || _panelW > 2048 || _panelH == 0 || _panelH > 2048) {
    _panelW = _fbW;
    _panelH = _fbH;
  }
  if (_imgBufAddr == 0) _imgBufAddr = _cfg.imgBufFallbackAddr;
#ifdef IT8951_PROBE_DEBUG
  // Diagnostic: raw GET_DEV_INFO read-back. Plausible values (e.g. 960x540, a
  // non-zero buffer addr, ASCII firmware string) mean SPI reads work; all-zero or
  // 0xFFFF means no MISO / cold controller / wrong pins.
  if (Serial)
    Serial.printf("[it8951] GET_DEV_INFO: panelW=%u panelH=%u imgBufAddr=0x%08lX (info[0..3]=%04X %04X %04X %04X)\n",
                  _panelW, _panelH, (unsigned long)_imgBufAddr, info[0], info[1], info[2], info[3]);
#endif
}

void It8951Driver::setTargetMemoryAddr(uint32_t addr) {
  writeReg(REG_LISAR + 2, static_cast<uint16_t>(addr >> 16));
  writeReg(REG_LISAR, static_cast<uint16_t>(addr & 0xFFFF));
}

void It8951Driver::setVcom(uint16_t mv) {
  if (mv == 0) return;  // keep factory OTP VCOM
  writeCommand(CMD_VCOM);
  writeData(0x0001);  // 1 = set
  writeData(mv);
}

void It8951Driver::displayArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode) {
  writeCommand(CMD_DPY_AREA);
  writeData(x);
  writeData(y);
  writeData(w);
  writeData(h);
  writeData(mode);
}

void It8951Driver::waitDisplayReady() {
  const unsigned long start = millis();
  while (readReg(REG_LUTAFSR) != 0) {
    if (millis() - start > READY_TIMEOUT_MS) {
#ifdef IT8951_PROBE_DEBUG
      if (Serial) Serial.printf("[it8951] waitDisplayReady TIMEOUT (LUTAFSR never cleared)\n");
#endif
      return;
    }
  }
}

// Expand the 1bpp landscape framebuffer to the IT8951's 4bpp packing and stream it
// into controller SRAM. White bit (1) -> 0xF, black bit (0) -> 0x0; two pixels per
// byte, leftmost pixel in the high nibble. The whole image rides one CS-low burst
// after a single PRE_WR preamble (per-word framing would be far too slow).
void It8951Driver::loadImageFull(const uint8_t* fb) {
  if (!_running) {
    systemRun();
    _running = true;
  }
  setTargetMemoryAddr(_imgBufAddr);

  const uint16_t arg = static_cast<uint16_t>((ENDIAN_BIG << 8) | (BPP_4 << 4) | (_rotation & 0x03));
  writeCommand(CMD_LD_IMG_AREA);
  writeData(arg);
  writeData(0);     // x
  writeData(0);     // y
  writeData(_fbW);  // w (image space)
  writeData(_fbH);  // h

  const uint16_t rowOutBytes = _fbW / 2;  // 4bpp -> 2 px per byte
  static uint8_t rowBuf[960 / 2];         // sized to the widest supported row (960 px)

  waitReady();
  _spi.beginTransaction(SPISettings(_cfg.spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer16(PRE_WR);
  for (uint16_t y = 0; y < _fbH; y++) {
    const uint8_t* src = fb + static_cast<uint32_t>(y) * _fbWb;
    uint16_t o = 0;
    for (uint16_t xb = 0; xb < _fbWb; xb++) {
      const uint8_t b = src[xb];
      rowBuf[o++] = static_cast<uint8_t>(((b & 0x80) ? 0xF0 : 0x00) | ((b & 0x40) ? 0x0F : 0x00));
      rowBuf[o++] = static_cast<uint8_t>(((b & 0x20) ? 0xF0 : 0x00) | ((b & 0x10) ? 0x0F : 0x00));
      rowBuf[o++] = static_cast<uint8_t>(((b & 0x08) ? 0xF0 : 0x00) | ((b & 0x04) ? 0x0F : 0x00));
      rowBuf[o++] = static_cast<uint8_t>(((b & 0x02) ? 0xF0 : 0x00) | ((b & 0x01) ? 0x0F : 0x00));
    }
    _spi.writeBytes(rowBuf, rowOutBytes);
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();

  writeCommand(CMD_LD_IMG_END);
}

// Same rotation/streaming path as loadImageFull, but each pixel's nibble is the
// 4-level gray reconstructed from the B/W base + the buffered LSB/MSB planes.
void It8951Driver::loadImageGray(const uint8_t* base) {
  if (!_running) {
    systemRun();
    _running = true;
  }
  setTargetMemoryAddr(_imgBufAddr);

  const uint16_t arg = static_cast<uint16_t>((ENDIAN_BIG << 8) | (BPP_4 << 4) | (_rotation & 0x03));
  writeCommand(CMD_LD_IMG_AREA);
  writeData(arg);
  writeData(0);     // x
  writeData(0);     // y
  writeData(_fbW);  // w (image space)
  writeData(_fbH);  // h

  const bool haveGray = _gLsb && _gMsb;
  const uint16_t rowOutBytes = _fbW / 2;
  static uint8_t rowBuf[960 / 2];

  waitReady();
  _spi.beginTransaction(SPISettings(_cfg.spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer16(PRE_WR);
  for (uint16_t y = 0; y < _fbH; y++) {
    const uint32_t rowOff = static_cast<uint32_t>(y) * _fbWb;
    const uint8_t* brow = base + rowOff;
    const uint8_t* lrow = haveGray ? _gLsb + rowOff : nullptr;
    const uint8_t* mrow = haveGray ? _gMsb + rowOff : nullptr;
    uint16_t o = 0;
    for (uint16_t xb = 0; xb < _fbWb; xb++) {
      const uint8_t bb = brow[xb];
      const uint8_t lb = haveGray ? lrow[xb] : 0;
      const uint8_t mb = haveGray ? mrow[xb] : 0;
      rowBuf[o++] = static_cast<uint8_t>((gray4(bb, lb, mb, 0x80) << 4) | gray4(bb, lb, mb, 0x40));
      rowBuf[o++] = static_cast<uint8_t>((gray4(bb, lb, mb, 0x20) << 4) | gray4(bb, lb, mb, 0x10));
      rowBuf[o++] = static_cast<uint8_t>((gray4(bb, lb, mb, 0x08) << 4) | gray4(bb, lb, mb, 0x04));
      rowBuf[o++] = static_cast<uint8_t>((gray4(bb, lb, mb, 0x02) << 4) | gray4(bb, lb, mb, 0x01));
    }
    _spi.writeBytes(rowBuf, rowOutBytes);
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();

  writeCommand(CMD_LD_IMG_END);
}

void It8951Driver::begin(EpdBus& bus) {
  (void)bus;  // external bus: this driver owns SPI
  const auto& d = BoardConfig::ACTIVE.display;
  _sclk = d.sclk;
  _mosi = d.mosi;
  _cs = d.cs;
  _busy = d.busy;
  _pwrEn = d.powerEnable;
  _miso = _cfg.miso;
  _sdCs = BoardConfig::ACTIVE.sd.cs;  // shared SPI bus: hold SD de-selected

  if (_cs >= 0) {
    pinMode(_cs, OUTPUT);
    digitalWrite(_cs, HIGH);
  }
  if (_sdCs >= 0) {
    pinMode(_sdCs, OUTPUT);
    digitalWrite(_sdCs, HIGH);
  }
  if (_busy >= 0) pinMode(_busy, INPUT);
  if (_pwrEn >= 0) {
    pinMode(_pwrEn, OUTPUT);
    digitalWrite(_pwrEn, HIGH);  // enable the EPD power rail
    delay(100);
  }

  // Bring up the shared global SPI bus. SDCardManager::begin() may have already
  // begun this same object with the SD pins; that's fine *only* because a
  // shared-bus board wires the display and SD on the same SCLK/MOSI/MISO, so both
  // begins program identical pins (whichever runs last is a no-op re-init). The
  // contract: on a shared bus (display.sclk == sd.sclk) the SPI pins must match —
  // otherwise the two begins fight and only one survives. Catch a profile that
  // violates it during bring-up.
#ifdef IT8951_PROBE_DEBUG
  const auto& sd = BoardConfig::ACTIVE.sd;
  if (sd.sclk >= 0 && _sclk == sd.sclk && (_mosi != sd.mosi || _miso != sd.miso) && Serial) {
    Serial.printf("[it8951] WARNING shared-bus pin mismatch: display sclk/mosi/miso=%d/%d/%d sd=%d/%d/%d\n", _sclk,
                  _mosi, _miso, sd.sclk, sd.mosi, sd.miso);
  }
#endif
  _spi.begin(_sclk, _miso, _mosi, -1);  // manual CS toggling

  // Grayscale plane buffers + B/W base snapshot (PSRAM): combined in displayGray().
  const size_t planeBytes = static_cast<size_t>(_fbWb) * _fbH;
  if (!_gLsb) _gLsb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
  if (!_gMsb) _gMsb = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));
  if (!_base) _base = static_cast<uint8_t*>(heap_caps_malloc(planeBytes, MALLOC_CAP_SPIRAM));

  waitReady();
  getDeviceInfo();
  writeReg(REG_I80CPCR, 0x0001);  // enable host packed write
  setVcom(_cfg.vcomMv);

  // Resolve rotation: AUTO picks 90° when the panel reports portrait so the
  // landscape framebuffer lands upright.
  _rotation = (_cfg.rotation == IT8951_ROTATE_AUTO) ? (_panelW < _panelH ? 1 : 0) : (_cfg.rotation & 0x03);
  _running = true;

  // Clear to white with the INIT waveform (ignores buffer content).
  displayArea(0, 0, _panelW, _panelH, 0);
  waitDisplayReady();
}

void It8951Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;  // IT8951 holds the previous frame in its own SRAM

  uint16_t dpyMode = _cfg.fastMode;
  if (mode == RefreshMode::Full) dpyMode = _cfg.fullMode;
  else if (mode == RefreshMode::Half) dpyMode = _cfg.halfMode;

#ifdef IT8951_PROBE_DEBUG
  if (Serial)
    Serial.printf("[it8951] display() mode=%d dpyMode=%u running=%d fb=%p panel=%ux%u turnOff=%d\n", (int)mode, dpyMode,
                  _running, fb, _panelW, _panelH, turnOff);
#endif

  // Snapshot this B/W frame: the consumer clears the live framebuffer during its
  // grayscale strip pass, so displayGray() needs this copy as the true base.
  if (_base && fb) memcpy(_base, fb, static_cast<size_t>(_fbWb) * _fbH);

  loadImageFull(fb);
  displayArea(0, 0, _panelW, _panelH, dpyMode);
  waitDisplayReady();

  if (turnOff) {
    writeCommand(CMD_STANDBY);  // park the controller; next load re-runs SYS_RUN
    _running = false;
  }
}

void It8951Driver::deepSleep(EpdBus& bus) {
  (void)bus;
  waitDisplayReady();
  writeCommand(CMD_SLEEP);
  _running = false;
}

// --- grayscale -------------------------------------------------------------
// The consumer renders the anti-aliasing LSB/MSB planes and hands them here; we
// buffer them and reconstruct the 16-gray image against the B/W base in
// displayGray(). Full-frame (copyGrayscale*) and per-strip (writeGrayscalePlaneStrip)
// delivery are both supported; the strip path is preferred because it leaves the
// consumer's B/W framebuffer intact, so displayGray() gets the true base buffer.
void It8951Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (_gLsb && lsb) memcpy(_gLsb, lsb, static_cast<size_t>(_fbWb) * _fbH);
}

void It8951Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  if (_gMsb && msb) memcpy(_gMsb, msb, static_cast<size_t>(_fbWb) * _fbH);
}

void It8951Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                            uint16_t numRows) {
  (void)bus;
  uint8_t* dst = (plane == GrayPlane::Lsb) ? _gLsb : _gMsb;
  if (!dst || !rows) return;
  memcpy(dst + static_cast<uint32_t>(yStart) * _fbWb, rows, static_cast<size_t>(numRows) * _fbWb);
}

void It8951Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) {
  (void)bus;
  (void)lut;
  (void)factoryMode;
  // The consumer's strip-grayscale pass clears the live framebuffer (fb) to 0x00,
  // so use the B/W snapshot captured in display() as the base. Fall back to the
  // passed buffer only if no snapshot exists yet.
  const uint8_t* base = _base ? _base : fb;
#ifdef IT8951_PROBE_DEBUG
  if (Serial && base) {
    const uint32_t mid = static_cast<uint32_t>(_fbH / 2) * _fbWb;
    Serial.printf("[it8951] displayGray() snapBase[0,mid]=%02X %02X fb[0]=%02X gLsb=%p gMsb=%p turnOff=%d\n", base[0],
                  base[mid], fb ? fb[0] : 0, _gLsb, _gMsb, turnOff);
  }
#endif
  loadImageGray(base);  // base = snapshot B/W; LSB/MSB already buffered
  // DU4 is a differential update: only pixels that changed (the AA glyph edges)
  // transition, so a full-panel refresh refines the edges without a flash.
  displayArea(0, 0, _panelW, _panelH, _cfg.grayMode);
  waitDisplayReady();
  if (turnOff) {
    writeCommand(CMD_STANDBY);
    _running = false;
  }
}

void It8951Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  // Nothing to re-sync: the IT8951 holds its own frame, and the next display()
  // reloads the framebuffer wholesale. The base B/W frame is left untouched.
  (void)bus;
  (void)bw;
}

// Per-board injection mirrors the other drivers: a board wiring the IT8951
// differently (MISO pin, panel VCOM, mount rotation) supplies its own config via
// -DFREEINK_IT8951_CONFIG=yourConfig without editing this driver.
#ifdef FREEINK_IT8951_CONFIG
const It8951Config& FREEINK_IT8951_CONFIG();
static const It8951Config& it8951ActiveConfig() { return FREEINK_IT8951_CONFIG(); }
#else
static const It8951Config& it8951ActiveConfig() { return it8951DefaultConfig(); }
#endif

PanelDriver& it8951Driver() {
  static It8951Driver instance(it8951ActiveConfig());
  return instance;
}

}  // namespace freeink

#endif  // FREEINK_DRIVER_IT8951
