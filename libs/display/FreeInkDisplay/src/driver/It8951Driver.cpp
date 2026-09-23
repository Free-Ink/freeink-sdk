#include "It8951Driver.h"

#include <BoardConfig.h>

// Linked only for the IT8951 devices (M5Paper v1.1 on the classic ESP32,
// e-Minimal 7.8" on the ESP32-S3). The body is plain Arduino SPIClass, so it is
// not tied to one MCU family.
#if FREEINK_DRIVER_IT8951

#include <driver/gpio.h>

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_system.h"

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
constexpr uint16_t REG_UP1SR = 0x1138;    // update parameter 1: +2 bit 2 = read frame memory as a 1bpp bitmap
constexpr uint16_t REG_LUTAFSR = 0x1224;  // LUT engine busy (0 = idle)
constexpr uint16_t REG_BGVR = 0x1250;     // bitmap colour table: [7:0] level for a 1 bit, [15:8] for a 0 bit

// LD_IMG arg fields: (endian << 8) | (bpp << 4) | rotation.
constexpr uint16_t BPP_2 = 0x00;       // 2 bits/pixel (four levels, expanded by the controller)
constexpr uint16_t BPP_4 = 0x02;       // 4 bits/pixel (native 16-gray)
constexpr uint16_t BPP_8 = 0x03;       // 8 bits/pixel (top nibble used) -- and the bitmap load's header
constexpr uint16_t ENDIAN_BIG = 0x01;  // big-endian pixel words

// 1bpp bitmap mode. A set framebuffer bit is white in this SDK, so a 1 bit
// paints 0xF0 and a 0 bit 0x00 -- the value Waveshare's and PaperTTY's 1bpp
// paths write, both of which also draw with 1 = white. If the glass comes up
// inverted, swap the bytes (0xF000).
constexpr uint16_t BGVR_BW = 0x00F0;
// The engine reads each byte of the bitmap LSB-first: pixel 0 of the eight is
// bit 0 (Waveshare's Paint lib packs 1bpp as `0x80 >> (7 - x%8)`, PaperTTY's
// pack_1bpp puts pixel 0 at value 1). The framebuffer is MSB-first, so every
// byte is bit-reversed on the way out. If text comes up scrambled inside
// 8-px groups, this is the assumption to flip (send the bytes as they are).
constexpr bool BITMAP_LSB_FIRST = true;
// Partial bitmap loads: PaperTTY only trusts them with the height a multiple
// of 16 (and x/w of 32; loads here are always full width). Bands are widened
// to 16-row boundaries, and the bitmap-mode refresh box to 32-px columns.
constexpr uint16_t BITMAP_ROW_ALIGN = 16;
constexpr uint16_t BITMAP_COL_ALIGN = 32;

struct BitReverseTable {
  uint8_t v[256];
  constexpr BitReverseTable() : v() {
    for (unsigned i = 0; i < 256; i++) {
      unsigned b = i;
      b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
      b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
      b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
      v[i] = static_cast<uint8_t>(b);
    }
  }
};
constexpr BitReverseTable kBitRev;

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

// The same pixel as a 2bpp level, 3/2/1/0 -- exactly gray4()'s four values if
// the controller expands v to v*5. The IT8951E on the eMinimal bench expands
// v<<2 (white = 0xC, grey), which is why It8951LoadDepth::Bpp2 is off there.
inline uint8_t gray2(uint8_t base, uint8_t lsb, uint8_t msb, uint8_t mask) {
  if (base & mask) return 3;  // white
  const bool l = lsb & mask, m = msb & mask;
  if (m && !l) return 2;  // light
  if (m || l) return 1;   // dark
  return 0;               // black
}
}  // namespace

const It8951Config& it8951DefaultConfig() {
  static const It8951Config cfg = {
      13,                   // miso (M5Paper: GPIO13, shared with SD)
      10000000,             // 10 MHz SPI
      IT8951_ROTATE_AUTO,   // pick 0°/90° from the reported panel orientation
      0,                    // vcomMv: 0 -> keep the panel's factory OTP VCOM
      2,                    // fullMode  = GC16 (ghost-clearing; Full/Half/wake/periodic)
      1,                    // fastMode  = DU   (2-level B/W, fast)
      6,                    // grayMode  = DU4  (4-level DIRECT update: differential,
                            //                   only changed pixels move -> no flash)
      8,                    // ghostClearInterval: GC16 clear every 8 differential refreshes
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

void It8951Driver::setBitmapMode(bool on, bool force) {
  if (!force && on == _bitmapModeOn) return;
  waitDisplayReady();  // never change how the engine reads memory under a running refresh
  const uint16_t up1sr2 = readReg(REG_UP1SR + 2);
  writeReg(REG_UP1SR + 2, on ? static_cast<uint16_t>(up1sr2 | (1u << 2)) : static_cast<uint16_t>(up1sr2 & ~(1u << 2)));
  if (on) writeReg(REG_BGVR, BGVR_BW);
  _bitmapModeOn = on;
#ifdef IT8951_PROBE_DEBUG
  if (Serial) Serial.printf("[it8951] bitmap mode %s (UP1SR+2 was %04X)\n", on ? "ON" : "off", up1sr2);
#endif
}

void It8951Driver::displayArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode) {
  // The engine must read the memory the way the last load wrote it. INIT
  // (mode 0) ignores the memory, but keeping the bit honest costs nothing.
  setBitmapMode(_memBitmap);
  if (_memBitmap && _rotation == 0) {
    // Widen the box to 32-px columns: a bitmap-mode refresh of a box starting
    // mid-word is untested (see BITMAP_COL_ALIGN); a few extra pixels of an
    // unchanged margin are invisible.
    const uint16_t x0 = static_cast<uint16_t>(x & ~(BITMAP_COL_ALIGN - 1));
    uint32_t x1 = (static_cast<uint32_t>(x) + w + BITMAP_COL_ALIGN - 1) & ~static_cast<uint32_t>(BITMAP_COL_ALIGN - 1);
    if (x1 > _panelW) x1 = _panelW;
    x = x0;
    w = static_cast<uint16_t>(x1 - x0);
  }
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
  loadImageArea(fb, 0, static_cast<uint16_t>(_fbWb - 1), 0, static_cast<uint16_t>(_fbH - 1));
}

// The rectangle case. LD_IMG_AREA takes x/w in pixels; a framebuffer byte is 8
// pixels, which keeps every edge on the 4-px word boundary the 4bpp packing
// wants. The controller places the rows at (x, y) in its frame memory, so the
// pixels outside the rectangle keep whatever the last load put there.
void It8951Driver::loadImageArea(const uint8_t* fb, uint16_t xb0, uint16_t xb1, uint16_t y0, uint16_t y1) {
  if (!_rowBuf) return;  // begin() could not allocate the row buffer; nothing to stream
  if (bitmapLoads() && xb0 == 0 && xb1 == _fbWb - 1) {
    loadImageBitmap(fb, y0, y1);
    return;
  }
  if (!_running) {
    systemRun();
    _running = true;
  }
#ifdef IT8951_TIMING_LOG
  const uint32_t tLoad = micros();
#endif
  setTargetMemoryAddr(_imgBufAddr);

  // Levels over a bitmap: the two formats cannot share the frame memory, so a
  // partial load in this format replaces the whole frame (and with it any gray).
  if (_memBitmap) {
    xb0 = 0;
    xb1 = static_cast<uint16_t>(_fbWb - 1);
    y0 = 0;
    y1 = static_cast<uint16_t>(_fbH - 1);
    _memHasGray = false;
  }
  const bool twoBpp = _cfg.loadDepth == It8951LoadDepth::Bpp2;
  const uint16_t widthBytes = static_cast<uint16_t>(xb1 - xb0 + 1);
  const uint16_t arg =
      static_cast<uint16_t>((ENDIAN_BIG << 8) | ((twoBpp ? BPP_2 : BPP_4) << 4) | (_rotation & 0x03));
  writeCommand(CMD_LD_IMG_AREA);
  writeData(arg);
  writeData(static_cast<uint16_t>(xb0 * 8));         // x (image space)
  writeData(y0);                                     // y
  writeData(static_cast<uint16_t>(widthBytes * 8));  // w
  writeData(static_cast<uint16_t>(y1 - y0 + 1));     // h

  uint8_t* rowBuf = _rowBuf;  // _fbW / 2 bytes, allocated in begin()
  // 4bpp: 2 px per byte, a framebuffer byte becomes 4; 2bpp: 4 px per byte, 2.
  const uint16_t rowOutBytes = static_cast<uint16_t>(widthBytes * (twoBpp ? 2 : 4));

  waitReady();
  _spi.beginTransaction(SPISettings(loadClockHz(), MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer16(PRE_WR);
  for (uint16_t y = y0; y <= y1; y++) {
    const uint8_t* src = fb + static_cast<uint32_t>(y) * _fbWb + xb0;
    uint16_t o = 0;
    if (twoBpp) {
      // Spread each 1-bit pixel to 2 bits (1 -> 11 white, 0 -> 00 black),
      // leftmost pixel in the top bits of the first byte.
      for (uint16_t xb = 0; xb < widthBytes; xb++) {
        const uint8_t b = src[xb];
        rowBuf[o++] = static_cast<uint8_t>(((b & 0x80) ? 0xC0 : 0) | ((b & 0x40) ? 0x30 : 0) |
                                           ((b & 0x20) ? 0x0C : 0) | ((b & 0x10) ? 0x03 : 0));
        rowBuf[o++] = static_cast<uint8_t>(((b & 0x08) ? 0xC0 : 0) | ((b & 0x04) ? 0x30 : 0) |
                                           ((b & 0x02) ? 0x0C : 0) | ((b & 0x01) ? 0x03 : 0));
      }
    } else {
      for (uint16_t xb = 0; xb < widthBytes; xb++) {
        const uint8_t b = src[xb];
        rowBuf[o++] = static_cast<uint8_t>(((b & 0x80) ? 0xF0 : 0x00) | ((b & 0x40) ? 0x0F : 0x00));
        rowBuf[o++] = static_cast<uint8_t>(((b & 0x20) ? 0xF0 : 0x00) | ((b & 0x10) ? 0x0F : 0x00));
        rowBuf[o++] = static_cast<uint8_t>(((b & 0x08) ? 0xF0 : 0x00) | ((b & 0x04) ? 0x0F : 0x00));
        rowBuf[o++] = static_cast<uint8_t>(((b & 0x02) ? 0xF0 : 0x00) | ((b & 0x01) ? 0x0F : 0x00));
      }
    }
    _spi.writeBytes(rowBuf, rowOutBytes);
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();

  writeCommand(CMD_LD_IMG_END);
  _memBitmap = false;
#ifdef IT8951_TIMING_LOG
  _loadUs += micros() - tLoad;
  _loadBytes += static_cast<uint32_t>(widthBytes) * (twoBpp ? 2 : 4) * (y1 - y0 + 1);
#endif
}

// The bitmap load is the trick Waveshare's EPD_IT8951_1bp_Refresh and
// PaperTTY's driver share: the packed 1-bit rows go up under an 8bpp header
// declaring the image a panel-width/8 "pixels" wide, so the load engine
// stores the bytes untouched at the start of each row of the (8-bit-pitch)
// frame memory, and the display engine with UP1SR's bitmap bit set reads
// them back as eight pixels each, painting the two levels from BGVR. No
// expansion, so black and white are exact -- the thing 2bpp could not do --
// and the whole frame is 328 KB instead of 1.31 MB.
//
// Rotation 0 only (bitmapLoads() checks): the rotator would rotate the byte
// image. Rows are widened to 16-row boundaries (see BITMAP_ROW_ALIGN); the
// extra rows carry the same content the memory already holds.
void It8951Driver::loadImageBitmap(const uint8_t* fb, uint16_t y0, uint16_t y1) {
  if (!_rowBuf || !fb) return;
  if (!_running) {
    systemRun();
    _running = true;
  }
#ifdef IT8951_TIMING_LOG
  const uint32_t tLoad = micros();
#endif
  setTargetMemoryAddr(_imgBufAddr);

  if (!_memBitmap) {
    // A bitmap over levels: replace the whole frame (see loadImageArea).
    y0 = 0;
    y1 = static_cast<uint16_t>(_fbH - 1);
    _memHasGray = false;
  } else {
    y0 = static_cast<uint16_t>(y0 & ~(BITMAP_ROW_ALIGN - 1));
    const uint32_t end = (static_cast<uint32_t>(y1) + BITMAP_ROW_ALIGN) & ~static_cast<uint32_t>(BITMAP_ROW_ALIGN - 1);
    y1 = static_cast<uint16_t>((end > _fbH ? _fbH : end) - 1);
  }

  const uint16_t arg = static_cast<uint16_t>((ENDIAN_BIG << 8) | (BPP_8 << 4) | 0);
  writeCommand(CMD_LD_IMG_AREA);
  writeData(arg);
  writeData(0);                                  // x, in bytes of the bitmap row
  writeData(y0);                                 // y
  writeData(_fbWb);                              // w, in bytes (234 on 1872 px)
  writeData(static_cast<uint16_t>(y1 - y0 + 1));  // h

#ifdef IT8951_PROBE_DEBUG
  if (Serial) Serial.printf("[it8951] bitmap load rows %u..%u (%u B/row)\n", y0, y1, _fbWb);
#endif

  uint8_t* rowBuf = _rowBuf;  // needs _fbWb bytes; allocated as _fbWb * 4
  waitReady();
  _spi.beginTransaction(SPISettings(loadClockHz(), MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer16(PRE_WR);
  for (uint16_t y = y0; y <= y1; y++) {
    const uint8_t* src = fb + static_cast<uint32_t>(y) * _fbWb;
    if (BITMAP_LSB_FIRST) {
      for (uint16_t xb = 0; xb < _fbWb; xb++) rowBuf[xb] = kBitRev.v[src[xb]];
      _spi.writeBytes(rowBuf, _fbWb);
    } else {
      memcpy(rowBuf, src, _fbWb);  // PSRAM -> internal RAM for the SPI FIFO
      _spi.writeBytes(rowBuf, _fbWb);
    }
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();

  writeCommand(CMD_LD_IMG_END);
  _memBitmap = true;
#ifdef IT8951_TIMING_LOG
  _loadUs += micros() - tLoad;
  _loadBytes += static_cast<uint32_t>(_fbWb) * (y1 - y0 + 1);
#endif
}

// Row-by-row memcmp against the snapshot, then the first and last differing byte
// of each differing row. ~330 KB read from each buffer, a few tens of ms from
// PSRAM -- small next to the 1.1 s push it saves when the box is a menu band.
bool It8951Driver::diffBox(const uint8_t* fb, uint16_t& xb0, uint16_t& xb1, uint16_t& y0, uint16_t& y1) const {
  bool any = false;
  for (uint16_t y = 0; y < _fbH; y++) {
    const uint8_t* a = fb + static_cast<uint32_t>(y) * _fbWb;
    const uint8_t* b = _base + static_cast<uint32_t>(y) * _fbWb;
    if (memcmp(a, b, _fbWb) == 0) continue;
    uint16_t lo = 0;
    while (a[lo] == b[lo]) lo++;
    uint16_t hi = static_cast<uint16_t>(_fbWb - 1);
    while (a[hi] == b[hi]) hi--;
    if (!any) {
      any = true;
      y0 = y;
      xb0 = lo;
      xb1 = hi;
    } else {
      if (lo < xb0) xb0 = lo;
      if (hi > xb1) xb1 = hi;
    }
    y1 = y;
  }
  return any;
}

void It8951Driver::buildGrayTable() {
  if (_grayTab) return;
  _grayTab = static_cast<uint16_t*>(heap_caps_malloc(4096 * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!_grayTab) return;  // loadImageGray falls back to gray4() per pixel
  for (uint16_t b = 0; b < 16; b++) {
    for (uint16_t l = 0; l < 16; l++) {
      for (uint16_t m = 0; m < 16; m++) {
        const uint8_t bb = static_cast<uint8_t>(b), lb = static_cast<uint8_t>(l), mb = static_cast<uint8_t>(m);
        const uint8_t hi = static_cast<uint8_t>((gray4(bb, lb, mb, 0x8) << 4) | gray4(bb, lb, mb, 0x4));
        const uint8_t lo = static_cast<uint8_t>((gray4(bb, lb, mb, 0x2) << 4) | gray4(bb, lb, mb, 0x1));
        _grayTab[(b << 8) | (l << 4) | m] = static_cast<uint16_t>((hi << 8) | lo);
      }
    }
  }
}

// Same rotation/streaming path as loadImageFull, but each pixel's nibble is the
// 4-level gray reconstructed from the B/W base + the buffered LSB/MSB planes.
void It8951Driver::loadImageGray(const uint8_t* base) {
  if (!_rowBuf) return;
  if (!_running) {
    systemRun();
    _running = true;
  }
#ifdef IT8951_TIMING_LOG
  const uint32_t tLoad = micros();
#endif
  setTargetMemoryAddr(_imgBufAddr);

  const bool twoBpp = _cfg.loadDepth == It8951LoadDepth::Bpp2;
  const uint16_t arg =
      static_cast<uint16_t>((ENDIAN_BIG << 8) | ((twoBpp ? BPP_2 : BPP_4) << 4) | (_rotation & 0x03));
  writeCommand(CMD_LD_IMG_AREA);
  writeData(arg);
  writeData(0);     // x
  writeData(0);     // y
  writeData(_fbW);  // w (image space)
  writeData(_fbH);  // h

  const bool haveGray = _gLsb && _gMsb;
  uint8_t* rowBuf = _rowBuf;  // _fbW / 2 bytes, allocated in begin()
  const uint16_t maxXb = _fbWb;
  const uint16_t rowOutBytes = static_cast<uint16_t>(maxXb * (twoBpp ? 2 : 4));

  waitReady();
  _spi.beginTransaction(SPISettings(loadClockHz(), MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer16(PRE_WR);
  for (uint16_t y = 0; y < _fbH; y++) {
    const uint32_t rowOff = static_cast<uint32_t>(y) * _fbWb;
    const uint8_t* brow = base + rowOff;
    const uint8_t* lrow = haveGray ? _gLsb + rowOff : nullptr;
    const uint8_t* mrow = haveGray ? _gMsb + rowOff : nullptr;
    uint16_t o = 0;
    if (twoBpp) {
      for (uint16_t xb = 0; xb < maxXb; xb++) {
        const uint8_t bb = brow[xb];
        const uint8_t lb = haveGray ? lrow[xb] : 0;
        const uint8_t mb = haveGray ? mrow[xb] : 0;
        rowBuf[o++] = static_cast<uint8_t>((gray2(bb, lb, mb, 0x80) << 6) | (gray2(bb, lb, mb, 0x40) << 4) |
                                           (gray2(bb, lb, mb, 0x20) << 2) | gray2(bb, lb, mb, 0x10));
        rowBuf[o++] = static_cast<uint8_t>((gray2(bb, lb, mb, 0x08) << 6) | (gray2(bb, lb, mb, 0x04) << 4) |
                                           (gray2(bb, lb, mb, 0x02) << 2) | gray2(bb, lb, mb, 0x01));
      }
    } else if (_grayTab && haveGray) {
      // Two table lookups per framebuffer byte: the high nibbles of the three
      // planes pick the first two output bytes, the low nibbles the next two.
      for (uint16_t xb = 0; xb < maxXb; xb++) {
        const uint8_t bb = brow[xb], lb = lrow[xb], mb = mrow[xb];
        const uint16_t hi = _grayTab[((bb & 0xF0) << 4) | (lb & 0xF0) | (mb >> 4)];
        const uint16_t lo = _grayTab[((bb & 0x0F) << 8) | ((lb & 0x0F) << 4) | (mb & 0x0F)];
        rowBuf[o++] = static_cast<uint8_t>(hi >> 8);
        rowBuf[o++] = static_cast<uint8_t>(hi);
        rowBuf[o++] = static_cast<uint8_t>(lo >> 8);
        rowBuf[o++] = static_cast<uint8_t>(lo);
      }
    } else {
      for (uint16_t xb = 0; xb < maxXb; xb++) {
        const uint8_t bb = brow[xb];
        const uint8_t lb = haveGray ? lrow[xb] : 0;
        const uint8_t mb = haveGray ? mrow[xb] : 0;
        rowBuf[o++] = static_cast<uint8_t>((gray4(bb, lb, mb, 0x80) << 4) | gray4(bb, lb, mb, 0x40));
        rowBuf[o++] = static_cast<uint8_t>((gray4(bb, lb, mb, 0x20) << 4) | gray4(bb, lb, mb, 0x10));
        rowBuf[o++] = static_cast<uint8_t>((gray4(bb, lb, mb, 0x08) << 4) | gray4(bb, lb, mb, 0x04));
        rowBuf[o++] = static_cast<uint8_t>((gray4(bb, lb, mb, 0x02) << 4) | gray4(bb, lb, mb, 0x01));
      }
    }
    _spi.writeBytes(rowBuf, rowOutBytes);
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();

  writeCommand(CMD_LD_IMG_END);
  _memBitmap = false;  // whole frame, levels format
#ifdef IT8951_TIMING_LOG
  _loadUs += micros() - tLoad;
  _loadBytes += static_cast<uint32_t>(_fbWb) * 4 * _fbH;
#endif
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
    // PowerManager::powerDownRailsForSleep() latches this rail LOW through deep
    // sleep (the hold survives wake) and a held pad silently ignores writes.
    // Release it first — this driver bypasses EpdBus, which does the same
    // release for the other panels.
    gpio_hold_dis(static_cast<gpio_num_t>(_pwrEn));
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
  // One 4bpp row for the load loops, sized to this panel (936 B on 1872 px,
  // 480 B on M5Paper's 960). Internal RAM: SPI writeBytes reads it every row.
  if (!_rowBuf) {
    const size_t rowBytes = static_cast<size_t>(_fbWb) * 4;
    _rowBuf = static_cast<uint8_t*>(heap_caps_malloc(rowBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!_rowBuf) _rowBuf = static_cast<uint8_t*>(malloc(rowBytes));
  }

  waitReady();
  // SYS_RUN before anything else. deepSleep() leaves the controller in CMD_SLEEP,
  // and on boards whose IT8951 rail is not switched and whose RST is unassigned
  // (eMinimal bench: HAT 5V straight off the devkit) it stays there across the
  // host's deep-sleep wake and even an EN reset — GET_DEV_INFO then reads nothing
  // (the fallback geometry hides it), every wait returns instantly and the glass
  // never changes until a power cycle. Only SYS_RUN wakes it; on a running
  // controller it is a no-op. Waveshare's and M5EPD's init order is the same.
  systemRun();
  getDeviceInfo();
  writeReg(REG_I80CPCR, 0x0001);  // enable host packed write
  setVcom(_cfg.vcomMv);
  // The memory is whatever the last run left (levels, or a bitmap from a run
  // with Bpp1), and so is the engine's bitmap bit: start both at levels. The
  // first load after INIT is always whole-frame, so no stale rows survive.
  _memBitmap = false;
  setBitmapMode(false, /*force=*/true);

  // Resolve rotation: AUTO picks 90° when the panel reports portrait so the
  // landscape framebuffer lands upright.
  _rotation = (_cfg.rotation == IT8951_ROTATE_AUTO) ? (_panelW < _panelH ? 1 : 0) : (_cfg.rotation & 0x03);
  _running = true;

  // Clear to white with the INIT waveform (ignores buffer content).
  displayArea(0, 0, _panelW, _panelH, 0);
  waitDisplayReady();
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && _cfg.wakeScrub != It8951WakeScrub::None) {
    // Waking with the sleep cover on the glass: INIT alone leaves faint
    // residue of dark art (most visible in dithered fields, and confirmed on
    // eMinimal's glass too). Drive the pigment through a second pass: an
    // explicit white GC16 between two INIT passes (the default), or just a
    // second INIT where the config says that is enough. Cold boots skip the
    // cost. _base is scratch here; the first display() re-snapshots it.
    const bool whitePass =
        _cfg.wakeScrub == It8951WakeScrub::WhiteGc16AndInit || _cfg.wakeScrub == It8951WakeScrub::WhiteGc16;
    if (_base && whitePass) {
      memset(_base, 0xFF, static_cast<size_t>(_fbWb) * _fbH);
      loadImageFull(_base);
      displayArea(0, 0, _panelW, _panelH, _cfg.fullMode);
      waitDisplayReady();
    }
    if (_cfg.wakeScrub != It8951WakeScrub::WhiteGc16) {
      displayArea(0, 0, _panelW, _panelH, 0);
      waitDisplayReady();
    }
  }
  _firstPaintPending = true;
  _memMirrorsBase = false;  // glass is white, memory is whatever it was: first paint reloads
  _memHasGray = false;
  _dirtyValid = false;
  buildGrayTable();
}

// A refresh clears ghosting when it's a Full/Half (the consumer's stronger
// refresh), a wake from standby (fresh image), or the periodic ghost-clear —
// otherwise it's the differential mode. DU/DU4 leave residue that accumulates
// across menu and activity navigation; promoting to GC16 every
// ghostClearInterval refreshes wipes it automatically, like the X3 driver, with
// no firmware involvement. `weight` is this refresh's share of a whole frame in
// 1/256ths: a partial-area menu move counts its box, so a scroll does not flash
// every eight presses the way eight page turns do.
uint16_t It8951Driver::resolveMode(RefreshMode mode, uint16_t differential, uint16_t weight, bool force) {
  const bool clear = force || (mode != RefreshMode::Fast) || !_running || _firstPaintPending ||
                     (_cfg.ghostClearInterval != 0 &&
                      _partialsSinceClear >= static_cast<uint32_t>(_cfg.ghostClearInterval) * FRAME_WEIGHT);
  _firstPaintPending = false;
  _partialsSinceClear = clear ? 0 : _partialsSinceClear + weight;
  _lastClear = clear;
  return clear ? _cfg.fullMode : differential;
}

// Any anti-aliased (non-white, non-black) pixel inside the box, judged from the
// buffered planes against the snapshot: a set base bit is white, a clear one
// with either plane bit set is a gray level.
bool It8951Driver::grayWithin(uint16_t xb0, uint16_t xb1, uint16_t y0, uint16_t y1) const {
  if (!_gLsb || !_gMsb || !_base) return false;
  const uint16_t n = static_cast<uint16_t>(xb1 - xb0 + 1);
  for (uint16_t y = y0; y <= y1; y++) {
    const uint32_t off = static_cast<uint32_t>(y) * _fbWb + xb0;
    const uint8_t* b = _base + off;
    const uint8_t* l = _gLsb + off;
    const uint8_t* m = _gMsb + off;
    for (uint16_t i = 0; i < n; i++) {
      if ((l[i] | m[i]) & ~b[i]) return true;
    }
  }
  return false;
}

// The box now holds plain B/W in the frame memory: forget the planes there so
// a later box over the same place is not mistaken for gray.
void It8951Driver::clearPlanesWithin(uint16_t xb0, uint16_t xb1, uint16_t y0, uint16_t y1) {
  if (!_memHasGray || !_gLsb || !_gMsb) return;
  const size_t n = static_cast<size_t>(xb1 - xb0 + 1);
  for (uint16_t y = y0; y <= y1; y++) {
    const uint32_t off = static_cast<uint32_t>(y) * _fbWb + xb0;
    memset(_gLsb + off, 0, n);
    memset(_gMsb + off, 0, n);
  }
}

void It8951Driver::dirtyUnion(uint16_t xb0, uint16_t xb1, uint16_t y0, uint16_t y1) {
  if (!_dirtyValid) {
    _dirtyXb0 = xb0;
    _dirtyXb1 = xb1;
    _dirtyY0 = y0;
    _dirtyY1 = y1;
    _dirtyValid = true;
    return;
  }
  if (xb0 < _dirtyXb0) _dirtyXb0 = xb0;
  if (xb1 > _dirtyXb1) _dirtyXb1 = xb1;
  if (y0 < _dirtyY0) _dirtyY0 = y0;
  if (y1 > _dirtyY1) _dirtyY1 = y1;
}

void It8951Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)bus;
  (void)prev;  // IT8951 holds the previous frame in its own SRAM
#ifdef IT8951_TIMING_LOG
  const uint32_t tEntry = micros();
  _loadUs = 0;
  _loadBytes = 0;
#endif

  _baseStaged = false;  // a plain B/W display supersedes any base waiting for gray planes

  // When the controller's frame memory mirrors the snapshot, diff the new frame
  // against it: the changed box is all that needs loading, and its share of the
  // frame is what this refresh counts toward the periodic ghost-clear. Needs no
  // rotation between framebuffer and panel (the box is in framebuffer
  // coordinates and DPY_AREA takes panel ones). An empty box means the memory
  // is already right.
  uint16_t xb0 = 0, xb1 = 0, y0 = 0, y1 = 0;
  const bool canDiff = _memMirrorsBase && _base && fb && _rotation == 0 && _panelW == _fbW && _panelH == _fbH;
  const bool haveBox = canDiff && diffBox(fb, xb0, xb1, y0, y1);
  const bool unchanged = canDiff && !haveBox;
  const bool wholeFrame = !haveBox || (xb0 == 0 && xb1 == _fbWb - 1 && y0 == 0 && y1 == _fbH - 1);
  bool largeBox = wholeFrame;
  if (haveBox && !wholeFrame) {
    const uint32_t boxArea = static_cast<uint32_t>(xb1 - xb0 + 1) * (y1 - y0 + 1);
    largeBox = boxArea * 2 >= static_cast<uint32_t>(_fbWb) * _fbH;
  }
  // Gray under the box: DU is a two-level waveform, and driving anti-aliased
  // text to white with it leaves a shadow of every glyph -- what "back to Home
  // from a book" looked like. The planes are still buffered, so this is known
  // before the load, and such a refresh is promoted to GC16 on the box.
  const bool grayInBox = haveBox && _memHasGray && grayWithin(xb0, xb1, y0, y1);

  // What this refresh is, and what it counts toward the periodic ghost-clear:
  //  - unchanged: nothing driven, counts nothing;
  //  - a partial box over B/W (a menu highlight move): DU on the box, counts
  //    nothing (residue there is mild and the next screen change clears it);
  //  - gray under the box: GC16 on the box; a whole-frame one is a full clear;
  //  - a large box (a screen change): DU, one frame's worth -- or GC16 if
  //    menu moves have left residue behind (dirty), which also resets it.
  bool forceClear = false, boxClear = false;
  uint16_t weight = FRAME_WEIGHT;
  if (unchanged) {
    weight = 0;
  } else if (grayInBox) {
    forceClear = true;
    boxClear = !largeBox;
  } else if (!largeBox) {
    weight = 0;
  } else if (_dirtyValid) {
    forceClear = true;
  }
  // A clear the host asked for, or that the driver owes (first paint, wake from
  // standby), resyncs everything. The others only need the box loaded.
  const bool requestedClear = (mode != RefreshMode::Fast) || !_running || _firstPaintPending;
  // A box-only GC16 leaves residue elsewhere, so it does not count as a clear
  // for the periodic cadence; it just takes the clearing waveform on its box.
  uint16_t dpyMode = resolveMode(mode, _cfg.fastMode, boxClear ? 0 : weight, forceClear && !boxClear);
  if (boxClear) dpyMode = _cfg.fullMode;

#ifdef IT8951_PROBE_DEBUG
  if (Serial)
    Serial.printf("[it8951] display() mode=%d dpyMode=%u n=%lu box=%d large=%d gray=%d dirty=%d\n", (int)mode, dpyMode,
                  (unsigned long)_partialsSinceClear, (int)haveBox, (int)largeBox, (int)grayInBox, (int)_dirtyValid);
#endif

  // Snapshot this B/W frame: the consumer clears the live framebuffer during its
  // grayscale strip pass, so displayGray() needs this copy as the true base.
  if (_base && fb) memcpy(_base, fb, static_cast<size_t>(_fbWb) * _fbH);

  if (boxClear) {
    loadImageBand(fb, y0, y1);
    displayArea(static_cast<uint16_t>(xb0 * 8), y0, static_cast<uint16_t>((xb1 - xb0 + 1) * 8),
                static_cast<uint16_t>(y1 - y0 + 1), dpyMode);
    clearPlanesWithin(0, static_cast<uint16_t>(_fbWb - 1), y0, y1);
  } else if (!_lastClear) {
    if (unchanged) {
      // Nothing changed: drive nothing. A DU pass over an identical frame is
      // not free -- it re-drives dithered fills two-level and leaves them
      // patchy, which is what Home looked like after its second, identical
      // paint following the GC16 out of a book.
    } else if (!wholeFrame) {
      loadImageBand(fb, y0, y1);
      displayArea(static_cast<uint16_t>(xb0 * 8), y0, static_cast<uint16_t>((xb1 - xb0 + 1) * 8),
                  static_cast<uint16_t>(y1 - y0 + 1), dpyMode);
      clearPlanesWithin(0, static_cast<uint16_t>(_fbWb - 1), y0, y1);
      if (!largeBox) dirtyUnion(xb0, xb1, y0, y1);
    } else {
      loadImageFull(fb);
      displayArea(0, 0, _panelW, _panelH, dpyMode);
      _memHasGray = false;
    }
  } else if (canDiff && !requestedClear) {
    // A clear with the memory already right outside the box: load the box and
    // GC16 the frame -- no whole-frame push. The periodic clear off a page or
    // a screen change re-drives everything; a clear owed to menu residue
    // could be narrower, but the residue is where the eye is anyway.
    if (haveBox) {
      loadImageBand(fb, y0, y1);
      clearPlanesWithin(0, static_cast<uint16_t>(_fbWb - 1), y0, y1);
      if (wholeFrame) _memHasGray = false;
    }
    displayArea(0, 0, _panelW, _panelH, dpyMode);
    _dirtyValid = false;
  } else {
    loadImageFull(fb);
    displayArea(0, 0, _panelW, _panelH, dpyMode);
    _memHasGray = false;
    _dirtyValid = false;
  }
  waitDisplayReady();
  _memMirrorsBase = _base && fb;
#ifdef IT8951_TIMING_LOG
  // "same frame": the diff found nothing to load; a refresh may still have run
  // (a clear that was owed), which the refresh+wait number shows.
  printTiming(unchanged ? "display (same frame)" : "display", tEntry, dpyMode);
#endif

  if (turnOff) {
    writeCommand(CMD_STANDBY);  // park the controller; next load re-runs SYS_RUN
    _running = false;
  }
}

// Combined base: nothing goes to the panel here. Snapshot the B/W frame and the
// refresh the host wanted for it; displayGray() folds the planes in and drives
// the glass once. If there is no snapshot buffer, fall back to a plain display so
// the page still appears (the gray pass then re-refreshes, as before).
void It8951Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  if (!_base || !fb) {
    display(bus, fb, nullptr, fallback, turnOff);
    return;
  }
  memcpy(_base, fb, static_cast<size_t>(_fbWb) * _fbH);
  _memMirrorsBase = false;  // the snapshot is ahead of the frame memory until displayGray() loads it
  _baseStaged = true;
  _stagedMode = fallback;
  _stagedTurnOff = turnOff;
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
#ifdef IT8951_TIMING_LOG
  const uint32_t tEntry = micros();
  _loadUs = 0;
  _loadBytes = 0;
#endif
#ifdef IT8951_PROBE_DEBUG
  if (Serial && base) {
    const uint32_t mid = static_cast<uint32_t>(_fbH / 2) * _fbWb;
    Serial.printf("[it8951] displayGray() snapBase[0,mid]=%02X %02X fb[0]=%02X gLsb=%p gMsb=%p turnOff=%d\n", base[0],
                  base[mid], fb ? fb[0] : 0, _gLsb, _gMsb, turnOff);
  }
#endif
  loadImageGray(base);  // base = snapshot B/W; LSB/MSB already buffered
  _memMirrorsBase = base == _base;
  _memHasGray = _gLsb && _gMsb;
  // Staged base (the normal path): this is the page's only refresh, so it takes
  // the mode the host asked for -- GC16 on its clearing cadence, else DU4, a
  // 4-level differential update that moves only the changed pixels, no flash.
  // Legacy path (a B/W display() already ran): refine the edges with DU4, or
  // GC16 if that pass was a clear so ghosting clears fully.
  uint16_t gmode;
  if (_baseStaged) {
    gmode = resolveMode(_stagedMode, _cfg.grayMode);
    turnOff = turnOff || _stagedTurnOff;
    _baseStaged = false;
  } else {
    gmode = (_lastClear || _firstPaintPending) ? _cfg.fullMode : _cfg.grayMode;
    _firstPaintPending = false;
  }
  displayArea(0, 0, _panelW, _panelH, gmode);
  waitDisplayReady();
#ifdef IT8951_TIMING_LOG
  printTiming("displayGray", tEntry, gmode);
#endif
  // A whole-frame GC16 leaves nothing to clear; a DU4 page leaves the whole
  // frame for the next periodic clear.
  if (gmode == _cfg.fullMode) {
    _dirtyValid = false;
  } else {
    dirtyUnion(0, static_cast<uint16_t>(_fbWb - 1), 0, static_cast<uint16_t>(_fbH - 1));
  }
  if (turnOff) {
    writeCommand(CMD_STANDBY);
    _running = false;
  }
}

#ifdef IT8951_TIMING_LOG
// One line per refresh: the bytes pushed and how long the push took, the
// waveform that ran and what the rest of the call cost (the refresh itself,
// plus any wait for a refresh still running on entry), and the format the
// frame memory holds afterwards. What the bench reads to put a number on a
// page turn or a menu move; the host's own "Page render" line brackets it.
void It8951Driver::printTiming(const char* what, uint32_t t0Us, uint16_t dpyMode) {
  if (!Serial) return;
  const uint32_t totalUs = micros() - t0Us;
  const uint32_t restUs = totalUs > _loadUs ? totalUs - _loadUs : 0;
  Serial.printf("[it8951] %s: load %lu KB %lu ms (%s), mode %u refresh+wait %lu ms, total %lu ms\n", what,
                (unsigned long)(_loadBytes / 1024), (unsigned long)(_loadUs / 1000), _memBitmap ? "1bpp" : "4bpp",
                dpyMode, (unsigned long)(restUs / 1000), (unsigned long)(totalUs / 1000));
}
#endif

void It8951Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  // Nothing to re-sync: the IT8951 holds its own frame, and the next display()
  // reloads the framebuffer wholesale. The base B/W frame is left untouched --
  // unless it is still only staged (the gray pass was skipped or failed), in
  // which case put it on the glass as plain B/W rather than lose the page.
  (void)bw;
  if (_baseStaged) {
    _baseStaged = false;
    display(bus, _base, nullptr, _stagedMode, _stagedTurnOff);
  }
}

// Per-board injection mirrors the other drivers: a board wiring the IT8951
// differently (MISO pin, panel VCOM, mount rotation) supplies its own config via
// -DFREEINK_IT8951_CONFIG=yourConfig without editing this driver.
#ifdef FREEINK_IT8951_CONFIG
const It8951Config& FREEINK_IT8951_CONFIG();
static const It8951Config& it8951ActiveConfig() { return FREEINK_IT8951_CONFIG(); }
#elif FREEINK_DEVICE_EMINIMAL
// Supplied by the SDK's BoardEMinimal78 library (add it to lib_deps): MISO 17,
// VCOM from the FPC, and the M841 LUT's mode numbers. The M5Paper default
// below would put MISO on this board's SD D0 and run grayscale through A2.
const It8951Config& eminimal78It8951Config();
static const It8951Config& it8951ActiveConfig() { return eminimal78It8951Config(); }
#else
static const It8951Config& it8951ActiveConfig() { return it8951DefaultConfig(); }
#endif

// Picture pages (XTC reader): after a dark page, a second GC16 over the new
// page, from what the frame memory already holds -- no reload (~0.55 s).
void It8951Driver::repeatLastRefresh(EpdBus& bus) {
  (void)bus;
  if (!_running) return;
  waitDisplayReady();
  displayArea(0, 0, _panelW, _panelH, _cfg.fullMode);
  waitDisplayReady();
}

PanelDriver& it8951Driver() {
  static It8951Driver instance(it8951ActiveConfig());
  return instance;
}

}  // namespace freeink

#endif  // FREEINK_DRIVER_IT8951
