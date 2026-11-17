#include "Ssd2677Driver.h"

#include <BoardConfig.h>

#if FREEINK_DEVICE_PICCO || FREEINK_DEVICE_STICKY

#include <cstring>

#include "../lut/Ssd2677Gray16Luts.h"
#include "../lut/Ssd2677Luts.h"
#include "esp_heap_caps.h"

namespace freeink {
namespace {

struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t delayMs;
  bool waitBusy;
  uint8_t data[9];
};

// SE0400NQW47 init list (stock table at DROM 0x3c8246b4, run by FUN_4200b868).
constexpr InitCmd kPiccoInit[] = {
    {0x00, 2, 10, false, {0x27, 0x09}},
    {0x83, 9, 0, false, {0x00, 0x00, 0x03, 0x1F, 0x00, 0xC8, 0x02, 0xA8, 0x01}},
    {0x01, 6, 0, false, {0x07, 0xF0, 0x22, 0x20, 0x78, 0x22}},
    {0x06, 4, 0, false, {0x0F, 0x8B, 0x93, 0xC1}},
    {0xE7, 1, 0, false, {0xC1}},
    {0x50, 1, 0, false, {0x77}},
    {0x62, 8, 0, false, {0x98, 0x98, 0x98, 0x75, 0xCA, 0xB2, 0x98, 0x7E}},
    {0xE0, 1, 0, false, {0x10}},
    {0x65, 4, 0, false, {0x00, 0x00, 0x00, 0x00}},
    {0x30, 1, 0, false, {0x04}},
    {0x82, 1, 0, false, {0x28}},
    {0xE9, 1, 0, false, {0x01}},
};

// Sticky SSD2677 init list (Seeed stock v1.2.0, ssd2677_init_otp, table at DROM
// 0x3c292494). No LUT upload: waveforms come from OTP (see latchOtpWaveform).
constexpr InitCmd kStickyInit[] = {
    {0x00, 2, 0, true, {0x2F, 0x0E}},
    {0x06, 4, 0, false, {0x0F, 0x8B, 0x93, 0xC1}},
    {0xE7, 1, 0, false, {0xC1}},
    {0x30, 1, 0, false, {0x08}},
    {0x50, 1, 0, false, {0x77}},
    {0x62, 8, 0, false, {0x76, 0x76, 0x76, 0x5A, 0x9D, 0x8A, 0x76, 0x62}},
    {0x61, 4, 0, false, {0x03, 0x20, 0x02, 0xA8}},
    {0xE0, 1, 0, false, {0x10}},
    {0x65, 4, 0, false, {0x00, 0x00, 0x00, 0x00}},
    {0xE9, 1, 0, true, {0x01}},
};

// Stock's OTP waveform codes (0xE6) for 21..30 C: full 0xEE, partial 0x19.
// Fixed room-temperature bucket. Stock reads the panel temperature
// (CMD 0x40, a half-duplex read the shared SPI bus can't do mid-run) and picks
// E8/EB/EE/F1 and 08/0F/19/23 by <11/11-20/21-30/>=31 C.
constexpr uint8_t OTP_FULL_ROOM = 0xEE;
constexpr uint8_t OTP_FAST_ROOM = 0x19;
constexpr uint8_t OTP_GRAY_ROOM = 0xFA;  // stock: 0xF1 at 15 C and below, else 0xFA
// Sticky text AA: stock has none for this panel, so the overlay pass uploads the
// Picco SE0400NQW47's stock AA waveform (slot 5) over the latched OTP one. It
// was tuned for the Picco's glass, not the Sticky's.
constexpr const unsigned char* kStickyAaLut = lut_picco_se0400_gray;
// Never a stock OTP code: makes the next latch re-run reset + init.
constexpr uint8_t LATCH_REINIT = 0xFF;

bool isSticky() {
#if FREEINK_DEVICE_STICKY
  return BoardConfig::ACTIVE.board == BoardConfig::Board::Sticky;
#else
  return false;
#endif
}

constexpr uint8_t CMD_DATA = 0x10;
constexpr uint8_t CMD_LUT = 0x20;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_REFRESH = 0x12;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint16_t LUT_LEN = 535;

// One SPI burst staging buffer shared by every frame upload (static: 1 KB of
// .bss instead of a 1 KB stack frame; uploads never overlap).
uint8_t g_txChunk[1024];

// 8 pixels of old + new -> two bytes of 2-bit pixels, old in the high bit,
// MSB-first (stock FUN_422869b4).
inline void packPixels(uint8_t oldByte, uint8_t newByte, uint8_t* out) {
  // Spread four bits into the even positions. A 16-byte flash table replaces
  // eight per-pixel shifts for every framebuffer byte, preserving wire order.
  static constexpr uint8_t spread[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15,
                                       0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55};
  out[0] = static_cast<uint8_t>((spread[oldByte >> 4] << 1) | spread[newByte >> 4]);
  out[1] = static_cast<uint8_t>((spread[oldByte & 15] << 1) | spread[newByte & 15]);
}

// 6-bit LUT count: value bits 0..5 on byte bits 4,7,6,1,0,3, active-low.
inline int lutCount(uint8_t v) {
  static constexpr uint8_t bits[6] = {4, 7, 6, 1, 0, 3};
  int n = 0;
  for (int i = 0; i < 6; ++i) n |= ((~v >> bits[i]) & 1) << i;
  return n;
}

}  // namespace

int ssd2677LutDose(const unsigned char* lut, unsigned table) {
  // Level byte, active-low: white drive on bits 5/3/1, black on bits 2/0/6 (see Ssd2677Luts.h).
  static constexpr uint8_t white[3] = {5, 3, 1}, black[3] = {2, 0, 6};
  int dose = 0;
  for (unsigned group = 0; group < 8; ++group) {
    const unsigned char* g = lut + 7 + table * 88 + group * 11;
    int net = 0;
    for (unsigned phase = 0; phase < 8; ++phase) {
      if (phase % 4 == 3) continue;
      const uint8_t levels = pgm_read_byte(&g[1 + phase / 4]);
      const int frames = lutCount(pgm_read_byte(&g[3 + phase]));
      if (!((levels >> black[phase % 4]) & 1)) net += frames;
      if (!((levels >> white[phase % 4]) & 1)) net -= frames;
    }
    dose += net * lutCount(pgm_read_byte(&g[0]));
  }
  return dose;
}

uint32_t Ssd2677Driver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 20000000;
}

PanelGeometry Ssd2677Driver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight;
  return {w, h, static_cast<uint16_t>(w / 8), static_cast<uint32_t>(w / 8) * h};
}

void Ssd2677Driver::initPanel(EpdBus& bus) {
  if (isSticky()) {
    // Stock reset: RST low 20 ms, then 10 ms before talking to the panel.
    const int8_t rst = bus.pins().rst;
    digitalWrite(rst, LOW);
    delay(20);
    digitalWrite(rst, HIGH);
    delay(10);
  } else {
    bus.reset();
  }
  bus.waitBusy(" SSD2677 reset");
  const InitCmd* list = isSticky() ? kStickyInit : kPiccoInit;
  const size_t count = isSticky() ? sizeof(kStickyInit) / sizeof(kStickyInit[0]) : sizeof(kPiccoInit) / sizeof(kPiccoInit[0]);
  for (size_t i = 0; i < count; ++i) {
    const InitCmd& c = list[i];
    bus.cmd(c.cmd);
    bus.data(c.data, c.len);
    if (c.delayMs) delay(c.delayMs);
    if (c.waitBusy) bus.waitBusy(" SSD2677 init");
  }
  // Sticky's final command already waits for completion. Keep the final
  // wait for init tables without a per-command wait on their last entry.
  if (!list[count - 1].waitBusy) bus.waitBusy(" SSD2677 init");
  _needsPowerOn = true;
  _latchedWaveform = 0;
}

// Sticky: select the OTP waveform for the next frame (stock
// ssd2677_latch_*_waveform). Stock re-runs reset + init whenever the waveform
// changes, then latches it before the frame data.
void Ssd2677Driver::latchOtpWaveform(EpdBus& bus, uint8_t code) {
  if (_latchedWaveform != 0 && _latchedWaveform != code) initPanel(bus);
  bus.cmd(0xE0);
  bus.data(0x12);
  bus.cmd(0xE6);
  bus.data(code);
  bus.cmd(0xA5);
  bus.waitBusy(" SSD2677 latch");
  delay(10);
  _latchedWaveform = code;
}

void Ssd2677Driver::begin(EpdBus& bus) {
  initPanel(bus);
  const PanelGeometry g = geometry();
  _textAa.allocate(true, g.widthBytes, g.height);

  _needsInitialFull = true;
  _grayPending = _grayMaskReady = false;
  // Retained old-plane: this panel takes old+new as one 2bpp burst, so the old
  // plane must exactly equal what is physically on the glass. The host can't
  // supply it — CrossPoint builds single-buffer (EINK_DISPLAY_SINGLE_BUFFER_MODE),
  // so `prev` is always null — so the driver keeps its own, exactly like stock
  // (its old buffer at obj+0x1020). Seed it white (0xFF), matching stock's
  // FUN_4200b07c(obj, 0xFF) in panel begin, so the first full clears any image.
  const uint32_t size = geometry().bufferSize;
  if (!_old) _old = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (_old) memset(_old, 0xFF, size);
}

void Ssd2677Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;  // unused: single-buffer host passes null; we track our own old plane
  exitGray16(bus);
  _textAa.invalidate();
  // First paint after init clears the panel with the full waveform (stock forces
  // mode 0 on the first frame).
  if (_needsInitialFull) mode = RefreshMode::Full;
  // Gray pixels left by the last gray pass start opposite their target so the
  // differential waveform drives each of them.
  if (_grayPending && _old && _lsb) {
    for (uint32_t i = 0; i < geometry().bufferSize; ++i)
      _old[i] = (_old[i] & ~_lsb[i]) | (~fb[i] & _lsb[i]);
  }
  _grayPending = false;
  if (isSticky()) {
    // OTP waveforms: a full refresh sends old == new (every pixel driven to its
    // level) and powers off; a fast one diffs against the retained old plane
    // and keeps the rails on (stock).
    // Differential refresh needs a real previous frame. On allocation failure,
    // send old == new with the full waveform so black pixels can still clear.
    const bool fast = mode == RefreshMode::Fast && _old != nullptr;
    latchOtpWaveform(bus, fast ? OTP_FAST_ROOM : OTP_FULL_ROOM);
    sendPacked(bus, fast ? _old : fb, fb);
    powerAndRefresh(bus, !fast || turnOff);
    if (_old) memcpy(_old, fb, geometry().bufferSize);
    _needsInitialFull = false;
    return;
  }
  // Old plane = our retained last-displayed frame (seeded white in begin). A
  // differential refresh needs old to match the glass; without this the panel
  // would see old==new on every incremental update and move nothing (the whole
  // reason non-full frames ghosted). _old == nullptr (alloc failed) falls back to
  // an all-white old plane in sendPacked.
  if (!_old) mode = RefreshMode::Full;
  // The first frame after a book page still erases reading-depth blacks, so it
  // keeps the reading waveform (reader menu, footnotes over a page).
  const bool reading = _readerRefresh || _afterReader;
  _afterReader = _readerRefresh;
  // BOOX presents book pages and UI screen redraws with mode 2 and powers off;
  // only small interaction updates (highlight moves) use mode 3 with the rails on.
  // A redraw changes over 1/8 of the frame.
  const uint32_t size = geometry().bufferSize;
  bool redraw = reading;
  if (mode == RefreshMode::Fast && !redraw && _old) {
    uint32_t changed = 0;
    for (uint32_t i = 0; i < size; ++i) changed += _old[i] != fb[i];
    redraw = changed > size / 8;
  }
  // Undriven blacks fade: on a dark background erased white text shows as deeper
  // black, and in UI dither old dots show through. Drive every black pixel as
  // white-to-black so old and fresh black match. In light mode only on UI redraws,
  // and only where the frame changed: re-driving static text and reader pages
  // builds up ghosts.
  if (mode == RefreshMode::Fast && (_darkBackground || (redraw && !reading)) && _old) {
    for (uint32_t i = 0; i < size; ++i)
      if (_darkBackground || _old[i] != fb[i]) _old[i] |= static_cast<uint8_t>(~fb[i]);
  }
  // Real old plane for every mode: old == new gives each pixel the no-change drive.
  sendPacked(bus, _old, fb);
  const auto* fastLut = redraw ? lut_picco_se0400_fast : lut_picco_se0400_mode3;
  const bool keepPower = mode == RefreshMode::Fast && !redraw;
  refreshWith(bus, mode == RefreshMode::Fast ? fastLut : lut_picco_se0400_full, !keepPower);
  if (_old) memcpy(_old, fb, size);  // new becomes old for the next frame
  _needsInitialFull = false;
}

void Ssd2677Driver::sendPacked(EpdBus& bus, const uint8_t* hi, const uint8_t* lo, bool exclusive) {
  const PanelGeometry g = geometry();
  uint8_t* const chunk = g_txChunk;
  // The Sticky panel scans rows bottom-to-top relative to the framebuffer
  // (stock mirrors in software); the Picco takes them in order.
  const bool reversed = isSticky();
  bus.cmd(CMD_DATA);
  auto txn = bus.beginTxn();
  uint16_t n = 0;
  for (uint16_t r = 0; r < g.height; ++r) {
    const uint32_t row = static_cast<uint32_t>(reversed ? g.height - 1 - r : r) * g.widthBytes;
    for (uint16_t xb = 0; xb < g.widthBytes; ++xb) {
      // A null plane is all white (0xFF), stock's begin-time seed.
      const uint8_t low = lo ? lo[row + xb] : 0xFF;
      const uint8_t high = hi ? hi[row + xb] : 0xFF;
      packPixels(exclusive ? static_cast<uint8_t>(high & ~low) : high, low, &chunk[n]);
      n += 2;
      if (n == sizeof(g_txChunk)) {
        txn.writeBytes(chunk, n);
        n = 0;
      }
    }
  }
  if (n) txn.writeBytes(chunk, n);
}

void Ssd2677Driver::powerOffBetweenFrames(EpdBus& bus) {
  if (_needsPowerOn) return;
  bus.cmd(CMD_POWER_OFF);
  bus.data(0x00);
  bus.waitBusy(" SSD2677 power off");
  _needsPowerOn = true;
}

void Ssd2677Driver::powerOn(EpdBus& bus) {
  if (_needsPowerOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" SSD2677 power on");
    _needsPowerOn = false;
  }
}

void Ssd2677Driver::powerAndRefresh(EpdBus& bus, bool turnOff) {
  powerOn(bus);
  bus.cmd(CMD_REFRESH);
  bus.data(0x00);
  bus.waitRefreshComplete("refresh");
  if (turnOff) powerOffBetweenFrames(bus);
}

void Ssd2677Driver::refreshWith(EpdBus& bus, const unsigned char* lut, bool turnOff) {
  bus.cmd(CMD_LUT);
  bus.data(lut, LUT_LEN);
  powerAndRefresh(bus, turnOff);
}

bool Ssd2677Driver::ensureLsb() {
  if (!_lsb) _lsb = static_cast<uint8_t*>(heap_caps_malloc(geometry().bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return _lsb != nullptr;
}

void Ssd2677Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (_textAa.active()) { _textAa.stage(0, lsb, 0, geometry().height); return; }
  if (!lsb) return;
  if (!_absoluteGray) ensureLsb();
  _grayMaskReady = false;
  if (_lsb) memcpy(_lsb, lsb, geometry().bufferSize);
}

void Ssd2677Driver::beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback,
                                   bool turnOff) {
  exitGray16(bus);
  if (mode == GrayscaleMode::Overlay && _readerRefresh && _textAa.ready()) {
    // The page is presented by displayGray; only a clean needs its own refresh.
    if (fallback != RefreshMode::Fast || _needsInitialFull) display(bus, fb, nullptr, fallback, false);
    _absoluteGray = false;
    _textAa.begin(fb);
    return;
  }
  _textAa.invalidate();
  _absoluteGray = mode == GrayscaleMode::Absolute;
  if (_absoluteGray) {
    _grayPending = false;
    // The caller still has the B/W base here. On allocation failure display it
    // before the renderer reuses its framebuffer for the two image planes.
    if ((!isSticky() && !_old) || !ensureLsb()) display(bus, fb, nullptr, RefreshMode::Full, true);
    return;
  }
  if (isSticky() && mode == GrayscaleMode::Overlay && fallback == RefreshMode::Fast && !_needsInitialFull && _old) {
    // Give every incoming ink pixel the stock white-to-black pulse, even if
    // the last B/W base called it black before AA lightened it. For incoming
    // white, keep the real prior bit so old ink clears and blank paper holds.
    // The synchronous display below replaces this transition mask with fb.
    const uint32_t size = geometry().bufferSize;
    for (uint32_t i = 0; i < size; ++i) _old[i] |= static_cast<uint8_t>(~fb[i]);
  }
  displayGrayscaleBase(bus, fb, fallback, turnOff);
}

void Ssd2677Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (_textAa.active()) { _textAa.stage(1, msb, 0, geometry().height); return; }
  if (!msb || !_lsb) return;
  if (_absoluteGray) {
    if (isSticky()) {
      latchOtpWaveform(bus, OTP_GRAY_ROOM);
      sendPacked(bus, msb, _lsb);
    } else if (_old) {
      // Stock powers on before the image upload. Stage it until presentation
      // so cancellation during plane upload never leaves the rails on. The MSB
      // approximates the new B/W state. Force a full refresh even on cancellation
      // because the previous B/W history is no longer available.
      memcpy(_old, msb, geometry().bufferSize);
      _needsInitialFull = true;
    }
    return;
  }
  // Sticky: the B/W base latched an OTP waveform (E0 12 / E6 / A5). Re-run reset
  // + init so the gray LUT runs from the same controller state as on the Picco,
  // which never latches one. Reset leaves the glass as it is.
  if (isSticky()) initPanel(bus);
  // Stock (0x4228b030): MSB mask in the high bit, LSB mask in the low bit.
  // The Picco's stock AA powers on before the upload (0x4200bad0).
  if (!isSticky()) powerOn(bus);
  // lut_picco_se0400_gray (both boards) lightens code 01 by 3 frames and code
  // 10 by 4 and leaves code 11 undriven. Dark pixels set both masks, so they
  // go out as 01; light pixels (MSB only) stay 10.
  sendPacked(bus, msb, _lsb, true);
  if (!isSticky()) {
    // The vendor retains the MSB mask to correct the next B/W transition.
    memcpy(_lsb, msb, geometry().bufferSize);
    _grayMaskReady = true;
  }
}

void Ssd2677Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)lut;
  (void)factoryMode;
  exitGray16(bus);
  if (_textAa.active()) {
    if (!_textAa.compose()) { _textAa.invalidate(); _needsInitialFull = true; return; }
    // Text codes: white plane in the high bit, gray / driven-white plane in the low bit.
    // Sticky: a latched OTP waveform is cleared by reset + init before a RAM
    // table runs, as for its overlay AA. The Picco powers on before the upload.
    if (!isSticky()) powerOn(bus);
    else if (_latchedWaveform != 0 && _latchedWaveform != LATCH_REINIT) initPanel(bus);
    sendPacked(bus, _textAa.high(), _textAa.low());
    refreshWith(bus, lut_picco_se0400_text, true);  // stock reader pages power off
    if (isSticky()) _latchedWaveform = LATCH_REINIT;
    const auto* text = lut_picco_se0400_text;
    _textAa.finish(ssd2677LutDose(text, 1), ssd2677LutDose(text, 2), ssd2677LutDose(text, 3), ssd2677LutDose(text, 4));
    // B/W history: the page's base, with its gray pixels flagged so the next
    // B/W frame drives each of them to its target.
    const uint32_t size = geometry().bufferSize;
    if (_old) memcpy(_old, _textAa.base(), size);
    if (_old && ensureLsb()) {
      for (uint32_t i = 0; i < size; ++i) _lsb[i] = static_cast<uint8_t>(_textAa.low()[i] & ~_textAa.high()[i]);
      _grayPending = true;
    } else {
      _needsInitialFull = true;
    }
    _grayMaskReady = false;
    _afterReader = true;  // the next B/W frame erases reading-depth ink
    return;
  }
  if (_absoluteGray) {
    _absoluteGray = false;
    if (!_lsb || (!isSticky() && !_old)) return;  // beginGrayscale presented the B/W fallback.
    // Stock powers off after the image. The glass now holds gray a differential
    // refresh can't clear, so the next B/W frame is a full one.
    if (isSticky()) {
      powerAndRefresh(bus, true);
    } else {
      powerOn(bus);
      sendPacked(bus, _old, _lsb);
      refreshWith(bus, lut_picco_se0400_image, turnOff);
    }
    _needsInitialFull = true;
    return;
  }
  if (isSticky()) {
    // Overlay (AA): the base is on the glass and the masks are in controller
    // RAM; nudge them gray with the uploaded AA waveform. That replaces the
    // latched OTP waveform, so the next frame re-runs init before latching.
    if (!_lsb) return;  // nothing staged; `fb` may hold a gray plane by now
    refreshWith(bus, kStickyAaLut, turnOff);
    _latchedWaveform = LATCH_REINIT;
    return;
  }
  if (!_lsb) {  // planes never staged: plain B/W
    // An inverted base is already on the glass; `fb` holds the logical frame.
    if (!_darkBackground) display(bus, fb, nullptr, RefreshMode::Full, turnOff);
    return;
  }
  refreshWith(bus, lut_picco_se0400_gray, true);
  _grayPending = _grayMaskReady;
  _grayMaskReady = false;
  // Preserve the base outside the gray mask; masked pixels are driven on the next frame.
}

void Ssd2677Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  // A skipped text pass still owes the caller its deferred B/W page.
  if (_textAa.active()) display(bus, bw ? bw : _textAa.base(), nullptr, RefreshMode::Fast, true);
}

void Ssd2677Driver::sendLevelCodes(EpdBus& bus, const uint8_t* levels, const uint8_t codes[16], uint8_t solid) {
  const PanelGeometry g = geometry();
  const uint8_t solidByte = static_cast<uint8_t>((solid & 3) * 0x55);
  uint8_t* const chunk = g_txChunk;
  bus.cmd(CMD_DATA);
  auto txn = bus.beginTxn();
  uint16_t n = 0;
  const uint32_t pairs = static_cast<uint32_t>(g.width / 2) * g.height;  // bytes of `levels`
  // Two level bytes (four pixels) -> one wire byte, MSB-first like packPixels.
  for (uint32_t i = 0; i < pairs; i += 2) {
    if (levels) {
      const uint8_t a = levels[i];
      const uint8_t b = levels[i + 1];
      chunk[n++] = static_cast<uint8_t>(codes[a >> 4] << 6 | codes[a & 15] << 4 | codes[b >> 4] << 2 | codes[b & 15]);
    } else {
      chunk[n++] = solidByte;
    }
    if (n == sizeof(g_txChunk)) {
      txn.writeBytes(chunk, n);
      n = 0;
    }
  }
  if (n) txn.writeBytes(chunk, n);
}

void Ssd2677Driver::exitGray16(EpdBus& bus) {
  if (!_gray16OnGlass) return;
  balanceGray16(bus);
  // Ink hysteresis leaves the balanced pixels gray, not white: the full
  // refresh armed here is what clears the page.
  if (_old) memset(_old, 0xFF, geometry().bufferSize);  // white once the full refresh has run
  _needsInitialFull = true;
}

void Ssd2677Driver::balanceGray16(EpdBus& bus) {
  if (!_gray16OnGlass) return;
  _gray16OnGlass = false;
  // Each level got deep + B frames toward black and W back; give the net back
  // toward white (deep balance 0/8/16/24, coarse 0/4/8/12, fine 0/1/2/3) so
  // the pixels end balanced.
  if (_g16.useBalanceDeep) {
    sendLevelCodes(bus, _gray16, _g16.balanceDeep);
    refreshWith(bus, lut_picco_se0400_gray16_deep_balance, false);
  }
  sendLevelCodes(bus, _gray16, _g16.balanceCoarse);
  refreshWith(bus, lut_picco_se0400_gray16_balance, false);
  sendLevelCodes(bus, _gray16, _g16.balanceFine);
  refreshWith(bus, lut_picco_se0400_gray16_pull, false);
}

bool Ssd2677Driver::setGray16Turn(uint8_t topUp, uint8_t scrubEvery) {
  if (!supportsGray16Image() || (topUp != 0 && topUp != 4 && topUp != 8 && topUp != 12)) return false;
  _g16TopUp = topUp;
  _g16ScrubEvery = scrubEvery;
  _g16FastTurns = 0;
  return true;
}

bool Ssd2677Driver::displayGray16Image(EpdBus& bus, const uint8_t* levels, bool turnOff, bool fast) {
  if (!supportsGray16Image() || !levels) return false;
  const PanelGeometry g = geometry();
  const uint32_t bytes = static_cast<uint32_t>(g.width / 2) * g.height;
  // The balance passes need each pixel's level after the caller has freed its
  // buffer. 192 KB at 800x480: PSRAM only (the Picco always has it), allocated
  // once and kept, like _old. No allocation, no image: without the copy the
  // drive could not be given back.
  if (!_gray16) _gray16 = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_gray16) {
    if (Serial) Serial.printf("[%lu] [EPD] gray16: no PSRAM for %lu-byte level copy\n", millis(), static_cast<unsigned long>(bytes));
    return false;
  }
  _textAa.invalidate();
  _grayPending = _grayMaskReady = _absoluteGray = false;
  if (fast && _gray16OnGlass && _g16FastTurns < _g16ScrubEvery) {
    // Fast turn: the old image gives its drive back (toward white, no flash),
    // its gray pixels optionally get a few more white frames, and the new one
    // is drawn straight over it.
    balanceGray16(bus);
    if (_g16TopUp) {
      static constexpr uint8_t kLevelCode[4] = {3, 1, 2, 0};
      uint8_t topUp[16];
      topUp[0] = 3;  // white pixels stay undriven
      for (int l = 1; l < 16; ++l) topUp[l] = kLevelCode[_g16TopUp / 4];
      sendLevelCodes(bus, _gray16, topUp);
      refreshWith(bus, lut_picco_se0400_gray16_balance, false);  // white 0/4/8/12
    }
    ++_g16FastTurns;
  } else {
    exitGray16(bus);  // a previous 16-level image gives its drive back first
    // Start from the state the calibration did: two stock full refreshes with
    // every pixel on code 11 (15 frames black, then 15 white).
    for (int i = 0; i < 2; ++i) {
      sendLevelCodes(bus, nullptr, nullptr, 3);
      refreshWith(bus, lut_picco_se0400_full, false);
    }
    _g16FastTurns = 0;
  }
  if (_g16.useDeep) {  // one contiguous drive: true black, and dark grays pulled back from it
    sendLevelCodes(bus, levels, _g16.deep);
    refreshWith(bus, lut_picco_se0400_gray16_deep, false);
  }
  sendLevelCodes(bus, levels, _g16.coarse);
  refreshWith(bus, lut_picco_se0400_gray16_coarse, false);
  sendLevelCodes(bus, levels, _g16.fine);
  refreshWith(bus, lut_picco_se0400_gray16_fine, false);
  sendLevelCodes(bus, levels, _g16.pull);
  refreshWith(bus, lut_picco_se0400_gray16_pull, turnOff);
  memcpy(_gray16, levels, bytes);
  _gray16OnGlass = true;
  // Anything that replaces the image goes through exitGray16 first, and the
  // B/W history is gone either way.
  _needsInitialFull = true;
  return true;
}

Ssd2677Driver::Gray16Codes Ssd2677Driver::defaultGray16Codes() {
  Gray16Codes c{};
  memcpy(c.deep, kGray16DeepCode, 16);
  memcpy(c.coarse, kGray16CoarseCode, 16);
  memcpy(c.fine, kGray16FineCode, 16);
  memcpy(c.pull, kGray16PullCode, 16);
  memcpy(c.balanceDeep, kGray16BalanceDeepCode, 16);
  memcpy(c.balanceCoarse, kGray16BalanceCoarseCode, 16);
  memcpy(c.balanceFine, kGray16BalanceFineCode, 16);
  c.useDeep = c.useBalanceDeep = false;
  for (int l = 0; l < 16; ++l) {
    c.useDeep |= c.deep[l] != 3;
    c.useBalanceDeep |= c.balanceDeep[l] != 3;
  }
  return c;
}

bool Ssd2677Driver::setGray16Levels(EpdBus& bus, const uint8_t (*paths)[3]) {
  if (!supportsGray16Image() || !paths) return false;
  // Pass k of a table: code 11 undriven, then 01 / 10 / 00 for the table's
  // three doses (coarse 4/8/12, fine and pull 1/2/3; see Ssd2677Gray16Luts.h).
  // The deep pass has one dose, 24 frames, on code 01; its give-back has
  // 8/16/24 so any net drive 0..39 splits exactly.
  static constexpr uint8_t kLevelCode[4] = {3, 1, 2, 0};
  static constexpr uint8_t kDeepBalance[4] = {0, 8, 16, 24};
  Gray16Codes c{};
  c.useDeep = c.useBalanceDeep = false;
  for (int l = 0; l < 16; ++l) {
    const uint8_t b = paths[l][0], w = paths[l][1], x = paths[l][2];
    if (b > 15 || w > 3 || (x != 0 && x != 24) || w > b + x) return false;  // balance only drives toward white
    const int net = x + b - w;
    // Give back the largest deep-balance dose that leaves 0..15 for coarse + fine.
    int give = 0;
    for (int k = 3; k >= 0; --k)
      if (kDeepBalance[k] <= net && net - kDeepBalance[k] <= 15) {
        give = k;
        break;
      }
    const int rest = net - kDeepBalance[give];
    if (rest < 0 || rest > 15) return false;  // unreachable for net 0..39; never index past the tables
    c.deep[l] = x ? kLevelCode[1] : kLevelCode[0];
    c.coarse[l] = kLevelCode[b / 4];
    c.fine[l] = kLevelCode[b % 4];
    c.pull[l] = kLevelCode[w];
    c.balanceDeep[l] = kLevelCode[give];
    c.balanceCoarse[l] = kLevelCode[rest / 4];
    c.balanceFine[l] = kLevelCode[rest % 4];
    c.useDeep |= x != 0;
    c.useBalanceDeep |= give != 0;
  }
  exitGray16(bus);  // the image on the glass is balanced with the table it was drawn with
  _g16 = c;
  return true;
}

void Ssd2677Driver::deepSleep(EpdBus& bus) {
  _textAa.invalidate();
  powerOffBetweenFrames(bus);
  // Stock (FUN_4200b608): 0x07 with the 0xA5 check code.
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
  _needsPowerOn = true;
  _needsInitialFull = true;
}

PanelDriver& ssd2677Driver() {
  static Ssd2677Driver instance;
  return instance;
}

}  // namespace freeink

#endif  // FREEINK_DEVICE_PICCO || FREEINK_DEVICE_STICKY
