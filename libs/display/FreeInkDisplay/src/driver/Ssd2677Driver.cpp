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
  settle(bus);
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
  settle(bus);
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
  _textBaseUp = false;  // a frame between the text base and its gray replaces both
  _textOnGlass = false;
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
  const bool keepPower = (mode == RefreshMode::Fast && !redraw) || _holdPower;
  refreshWith(bus, mode == RefreshMode::Fast ? fastLut : lut_picco_se0400_full, !keepPower);
  if (_old) memcpy(_old, fb, size);  // new becomes old for the next frame
  _needsInitialFull = false;
}

void Ssd2677Driver::sendPacked(EpdBus& bus, const uint8_t* hi, const uint8_t* lo, bool exclusive) {
  settle(bus);
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
  settle(bus);
  if (_needsPowerOn) return;
  bus.cmd(CMD_POWER_OFF);
  bus.data(0x00);
  bus.waitBusy(" SSD2677 power off");
  _needsPowerOn = true;
}

void Ssd2677Driver::powerOn(EpdBus& bus) {
  settle(bus);
  if (_needsPowerOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" SSD2677 power on");
    _needsPowerOn = false;
  }
}

void Ssd2677Driver::settle(EpdBus& bus) {
  if (!_refreshInFlight) return;
  _refreshInFlight = false;
  // Still driving, or fired so recently that BUSY may not have asserted yet:
  // the normal completion wait. Otherwise the waveform is long done.
  if (bus.isBusy() || millis() - _refreshStartMs < 50) bus.waitRefreshComplete("refresh");
}

void Ssd2677Driver::powerAndRefresh(EpdBus& bus, bool turnOff) {
  powerOn(bus);
  bus.cmd(CMD_REFRESH);
  bus.data(0x00);
  if (_deferRefresh && !turnOff) {
    // The waveform runs on its own: return now and let the host work (render
    // the text tones) until the next bus access settles it.
    _refreshInFlight = true;
    _refreshStartMs = millis();
    return;
  }
  bus.waitRefreshComplete("refresh");
  if (turnOff) powerOffBetweenFrames(bus);
}

void Ssd2677Driver::refreshWith(EpdBus& bus, const unsigned char* lut, bool turnOff) {
  settle(bus);
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
  settle(bus);
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

// --- 16-level gray ------------------------------------------------------------
namespace {

// 535-byte table layout (Ssd2677Luts.h): 7-byte header, six 88-byte tables of
// eight 11-byte groups; table 0 VCOM, tables 1..4 pixel codes 00/01/10/11.
constexpr int kHeaderLen = 7, kTableLen = 88, kGroupLen = 11;
uint8_t* lutGroup(uint8_t* lut, int table, int group) { return lut + kHeaderLen + table * kTableLen + group * kGroupLen; }

// 6-bit counts, active-low, value bits 0..5 on byte bits 4,7,6,1,0,3.
uint8_t encodeCount(uint8_t n) {
  static constexpr uint8_t bits[6] = {4, 7, 6, 1, 0, 3};
  uint8_t v = 0xFF;
  for (int i = 0; i < 6; ++i)
    if ((n >> i) & 1) v = static_cast<uint8_t>(v & ~(1u << bits[i]));
  return v;
}
// Level byte, active-low: phase k (0..2) of a half drives white on bit 5/3/1, black on 2/0/6.
constexpr uint8_t kWhiteBit[3] = {5, 3, 1}, kBlackBit[3] = {2, 0, 6};

// Header and VCOM drive from the stock partial table, every group of tables
// 0..4 cleared (an all-0xFF group has no repeats and no frames).
void startTable(uint8_t* out) {
  memcpy(out, lut_picco_se0400_mode3, LUT_LEN);
  for (int t = 0; t < 5; ++t)
    for (int g = 0; g < 8; ++g) memset(lutGroup(out, t, g), 0xFF, kGroupLen);
}

void writeGroup(uint8_t* out, int table, int group, uint8_t lv0, uint8_t lv1, const uint8_t lens[8]) {
  uint8_t* g = lutGroup(out, table, group);
  g[0] = encodeCount(1);
  g[1] = lv0;
  g[2] = lv1;
  for (int p = 0; p < 8; ++p) g[3 + p] = encodeCount(lens[p]);
}

// One group of binary-weighted phases; code c driven dose[c] frames (black or
// white). Same bytes as picco-gray-cal lut.build_pass(trim=True).
void buildDosePass(uint8_t* out, const uint8_t dose[4], bool towardBlack) {
  static constexpr uint8_t kPhase[6] = {0, 1, 2, 4, 5, 6}, kWeight[6] = {32, 16, 8, 4, 2, 1};
  startTable(out);
  uint8_t used = 0;
  for (int c = 0; c < 4; ++c) used |= dose[c];
  uint8_t lens[8] = {};
  for (int i = 0; i < 6; ++i) lens[kPhase[i]] = (used & kWeight[i]) ? kWeight[i] : 0;
  lens[3] = lens[7] = 1;  // a half only runs when its last phase is non-zero
  const uint8_t* vcom = reinterpret_cast<const uint8_t*>(lut_picco_se0400_mode3) + kHeaderLen;
  writeGroup(out, 0, 0, vcom[1], vcom[2], lens);
  for (int c = 0; c < 4; ++c) {
    uint8_t lv[2] = {0xFF, 0xFF};
    for (int i = 0; i < 6; ++i) {
      if (!(dose[c] & kWeight[i])) continue;
      const int p = kPhase[i];
      lv[p / 4] = static_cast<uint8_t>(lv[p / 4] & ~(1u << (towardBlack ? kBlackBit[p % 4] : kWhiteBit[p % 4])));
    }
    writeGroup(out, 1 + c, 0, lv[0], lv[1], lens);
  }
}

// Turn dissolve (after PR #220's cross-dissolve): group 0 `erase` frames
// toward white, group 1 `deep` frames toward black. Codes 11 nothing, 01
// erase, 10 deep, 00 erase then deep. Same bytes as lut.build_dissolve().
void buildDissolve(uint8_t* out, uint8_t erase, uint8_t deep) {
  startTable(out);
  const uint8_t* vcom = reinterpret_cast<const uint8_t*>(lut_picco_se0400_mode3) + kHeaderLen;
  const uint8_t n[2] = {erase, deep};
  for (int g = 0; g < 2; ++g) {
    const uint8_t lens[8] = {n[g], 0, 0, 1, 0, 0, 0, 0};
    writeGroup(out, 0, g, vcom[1], vcom[2], lens);
    for (int c = 0; c < 4; ++c) {
      uint8_t lv0 = 0xFF;
      if (g == 0 && (c == 1 || c == 0)) lv0 = static_cast<uint8_t>(lv0 & ~(1u << kWhiteBit[0]));
      if (g == 1 && (c == 2 || c == 0)) lv0 = static_cast<uint8_t>(lv0 & ~(1u << kBlackBit[0]));
      writeGroup(out, 1 + c, g, lv0, 0xFF, lens);
    }
  }
}

// Text page turn, after PR #220's single-pass cross-dissolve: erase and new
// ink run at the same time instead of one group after the other. Code 01: old
// ink to white, `erase` frames; code 10: new deep black, `deep` frames; code
// 00: deep black then `pull` frames back toward white straight after, so the
// lightest deep level is gray by the end of the turn instead of black until
// the pull pass (the black flash); 11 undriven. Pixels with old ink that turn
// deep go straight to black (no erase). One group.
void buildCrossDissolve(uint8_t* out, uint8_t erase, uint8_t deep, uint8_t pull) {
  startTable(out);
  const uint8_t shortN = erase < deep ? erase : deep, longN = erase < deep ? deep : erase;
  const uint8_t lens[8] = {shortN, static_cast<uint8_t>(longN - shortN), pull, 1, 0, 0, 0, 0};
  const uint8_t* vcom = reinterpret_cast<const uint8_t*>(lut_picco_se0400_mode3) + kHeaderLen;
  writeGroup(out, 0, 0, vcom[1], vcom[2], lens);
  const bool eraseLong = erase > deep;
  uint8_t white = static_cast<uint8_t>(0xFF & ~(1u << kWhiteBit[0]));
  if (eraseLong) white = static_cast<uint8_t>(white & ~(1u << kWhiteBit[1]));
  uint8_t black = static_cast<uint8_t>(0xFF & ~(1u << kBlackBit[0]));
  if (!eraseLong) black = static_cast<uint8_t>(black & ~(1u << kBlackBit[1]));
  const uint8_t blackThenPull = static_cast<uint8_t>(black & ~(1u << kWhiteBit[2]));
  writeGroup(out, 1 + 1, 0, white, 0xFF, lens);          // 01
  writeGroup(out, 1 + 2, 0, black, 0xFF, lens);          // 10
  writeGroup(out, 1 + 0, 0, blackThenPull, 0xFF, lens);  // 00
  writeGroup(out, 1 + 3, 0, 0xFF, 0xFF, lens);           // 11
}

constexpr uint8_t kLevelCode[4] = {3, 1, 2, 0};  // dose index -> pixel code; 11 undriven
constexpr uint8_t kBalDeep[4] = {0, 8, 16, 24};    // give-back tables (Ssd2677Gray16Luts.h)

}  // namespace

bool Ssd2677Driver::computeGray16Codes(const uint8_t (*paths)[3], const uint8_t black[3], const uint8_t white[3],
                                       Gray16Codes& c) {
  c = Gray16Codes{};
  for (int l = 0; l < 16; ++l) {
    const uint8_t bi = paths[l][0], wi = paths[l][1], d = paths[l][2];
    if (bi > 3 || wi > 3 || d > 1) return false;
    const int net = d * kGray16Deep + (bi ? black[bi - 1] : 0) - (wi ? white[wi - 1] : 0);
    if (net < 0 || net > 39) return false;  // the give-back covers 0..39 frames toward black
    int give = 0;  // largest deep give-back leaving 0..15 for coarse + fine
    for (int k = 3; k >= 0; --k)
      if (kBalDeep[k] <= net && net - kBalDeep[k] <= 15) {
        give = k;
        break;
      }
    const int rest = net - kBalDeep[give];
    c.deep[l] = d ? 2 : 3;  // code 10 = deep black in the dissolve tables
    c.coarse[l] = kLevelCode[bi];
    c.pull[l] = kLevelCode[wi];
    c.balanceDeep[l] = kLevelCode[give];
    c.balanceCoarse[l] = kLevelCode[rest / 4];
    c.balanceFine[l] = kLevelCode[rest % 4];
    c.ink[l] = bi || wi || d;
    c.useDeep |= d != 0;
    c.useCoarse |= bi != 0;
    c.useBalanceDeep |= give != 0;
  }
  return true;
}

void Ssd2677Driver::buildGray16Tables() {
  const uint8_t black[4] = {0, _g16Black[0], _g16Black[1], _g16Black[2]};
  const uint8_t white[4] = {0, _g16White[0], _g16White[1], _g16White[2]};
  // Dose index k maps to code kLevelCode[k]; the builders index doses by code.
  uint8_t byCodeB[4], byCodeW[4];
  for (int k = 0; k < 4; ++k) {
    byCodeB[kLevelCode[k]] = black[k];
    byCodeW[kLevelCode[k]] = white[k];
  }
  buildDosePass(_lutCoarse, byCodeB, true);
  buildDosePass(_lutPull, byCodeW, false);
  buildDissolve(_lutDissolve, _g16Erase, kGray16Deep);
  buildDissolve(_lutDeep, 0, kGray16Deep);
  // The soft deep level: deep black, then the largest pull-back.
  _textSoftLevel = 0xFF;
  for (int l = 0; l < 16; ++l)
    if (_g16Paths[l][2] && _g16Paths[l][1] == 3 && !_g16Paths[l][0]) _textSoftLevel = static_cast<uint8_t>(l);
  if (_textSoftPull == 0xFF) _textSoftPull = _g16White[2];  // default: the level's own pull-back
  if (!_textSoftPull) _textSoftLevel = 0xFF;  // 0: pulled back by the pull pass instead
  buildCrossDissolve(_lutCross, _g16Erase, kGray16Deep, _textSoftLevel != 0xFF ? _textSoftPull : 0);
}

void Ssd2677Driver::ensureGray16Config() {
  if (_g16Ready) return;
  memcpy(_g16Black, kGray16DefaultBlack, 3);
  memcpy(_g16White, kGray16DefaultWhite, 3);
  _g16Erase = kGray16DefaultErase;
  memcpy(_g16Paths, kGray16DefaultPaths, sizeof(_g16Paths));
  computeGray16Codes(_g16Paths, _g16Black, _g16White, _g16);  // the generator checked the defaults
  buildGray16Tables();
  _g16Ready = true;
}

void Ssd2677Driver::sendDissolveCodes(EpdBus& bus, const uint8_t* oldLevels, const uint8_t* newLevels) {
  settle(bus);
  const PanelGeometry g = geometry();
  uint8_t* const chunk = g_txChunk;
  auto code = [this](uint8_t o, uint8_t n) -> uint8_t {
    const bool ink = _g16.ink[o], deep = _g16.deep[n] == 2;
    return ink ? (deep ? 0 : 1) : (deep ? 2 : 3);
  };
  bus.cmd(CMD_DATA);
  auto txn = bus.beginTxn();
  uint16_t n = 0;
  const uint32_t bytes = static_cast<uint32_t>(g.width / 2) * g.height;
  for (uint32_t i = 0; i < bytes; i += 2) {
    const uint8_t oa = oldLevels[i], ob = oldLevels[i + 1], na = newLevels[i], nb = newLevels[i + 1];
    chunk[n++] = static_cast<uint8_t>(code(oa >> 4, na >> 4) << 6 | code(oa & 15, na & 15) << 4 |
                                      code(ob >> 4, nb >> 4) << 2 | code(ob & 15, nb & 15));
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
  // Each level got deep + coarse frames toward black and pull-back frames to
  // white; give the net back toward white (deep 0/8/16/24, coarse 0/4/8/12,
  // fine 0/1/2/3) so the pixels end balanced.
  if (_g16.useBalanceDeep) {
    sendLevelCodes(bus, _gray16, _g16.balanceDeep);
    refreshWith(bus, lut_picco_se0400_gray16_deep_balance, false);
  }
  sendLevelCodes(bus, _gray16, _g16.balanceCoarse);
  refreshWith(bus, lut_picco_se0400_gray16_balance, false);
  sendLevelCodes(bus, _gray16, _g16.balanceFine);
  refreshWith(bus, lut_picco_se0400_gray16_balance_fine, false);
}

bool Ssd2677Driver::setGray16Turn(uint8_t scrubEvery) {
  if (!supportsGray16Image()) return false;
  _g16ScrubEvery = scrubEvery;
  _g16FastTurns = 0;
  return true;
}

bool Ssd2677Driver::setGray16Passes(EpdBus& bus, const uint8_t black[3], const uint8_t white[3], uint8_t erase) {
  if (!supportsGray16Image() || !black || !white || erase > 63) return false;
  for (int k = 0; k < 3; ++k)
    if (black[k] < 1 || black[k] > 63 || white[k] < 1 || white[k] > 63) return false;
  ensureGray16Config();
  Gray16Codes c;
  if (!computeGray16Codes(_g16Paths, black, white, c)) return false;
  exitGray16(bus);  // the image on the glass is balanced with the doses it was drawn with
  memcpy(_g16Black, black, 3);
  memcpy(_g16White, white, 3);
  _g16Erase = erase;
  _g16 = c;
  buildGray16Tables();
  return true;
}

bool Ssd2677Driver::setGray16Levels(EpdBus& bus, const uint8_t (*paths)[3]) {
  if (!supportsGray16Image() || !paths) return false;
  ensureGray16Config();
  Gray16Codes c;
  if (!computeGray16Codes(paths, _g16Black, _g16White, c)) return false;
  exitGray16(bus);  // the image on the glass is balanced with the table it was drawn with
  memcpy(_g16Paths, paths, sizeof(_g16Paths));
  _g16 = c;
  return true;
}

bool Ssd2677Driver::displayGray16Image(EpdBus& bus, const uint8_t* levels, bool turnOff, bool fast) {
  if (!supportsGray16Image() || !levels) return false;
  const PanelGeometry g = geometry();
  const uint32_t bytes = static_cast<uint32_t>(g.width / 2) * g.height;
  // The balance passes and the next turn's dissolve need each pixel's level
  // after the caller has freed its buffer. 192 KB at 800x480: PSRAM only (the
  // Picco always has it), allocated once and kept, like _old. No allocation,
  // no image: without the copy the drive could not be given back.
  if (!_gray16) _gray16 = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_gray16) {
    if (Serial) Serial.printf("[%lu] [EPD] gray16: no PSRAM for %lu-byte level copy\n", millis(), static_cast<unsigned long>(bytes));
    return false;
  }
  ensureGray16Config();
  _textAa.invalidate();
  _grayPending = _grayMaskReady = _absoluteGray = false;
  if (fast && _gray16OnGlass && _g16FastTurns < _g16ScrubEvery) {
    // Fast turn: one dissolve erases the old image's ink and lays the new deep
    // blacks, no full clears. The fixed erase does not return each pixel's
    // exact drive; the full turn every _g16ScrubEvery turns resets the glass.
    sendDissolveCodes(bus, _gray16, levels);
    refreshWith(bus, _lutDissolve, false);
    _gray16OnGlass = false;
    ++_g16FastTurns;
  } else {
    exitGray16(bus);  // a previous 16-level image gives its drive back first
    // Start from the state the calibration did: two stock full refreshes with
    // every pixel on code 11 (15 frames black, then 15 white).
    for (int i = 0; i < 2; ++i) {
      sendLevelCodes(bus, nullptr, nullptr, 3);
      refreshWith(bus, lut_picco_se0400_full, false);
    }
    if (_g16.useDeep) {  // one contiguous drive: the true black, and dark grays pulled back from it
      sendLevelCodes(bus, levels, _g16.deep);
      refreshWith(bus, _lutDeep, false);
    }
    _g16FastTurns = 0;
  }
  if (_g16.useCoarse) {
    sendLevelCodes(bus, levels, _g16.coarse);
    refreshWith(bus, _lutCoarse, false);
  }
  sendLevelCodes(bus, levels, _g16.pull);
  refreshWith(bus, _lutPull, turnOff);
  memcpy(_gray16, levels, bytes);
  _gray16OnGlass = true;
  _textOnGlass = false;
  // Anything that replaces the image goes through exitGray16 first, and the
  // B/W history is gone either way.
  _needsInitialFull = true;
  return true;
}

bool Ssd2677Driver::setGray16TextSoftPull(uint8_t frames) {
  if (!supportsGray16Image() || frames > 63) return false;
  ensureGray16Config();
  _textSoftPull = frames;
  buildGray16Tables();
  return true;
}

bool Ssd2677Driver::beginGray16Text(EpdBus& bus, const uint8_t* base, const uint8_t* soft) {
  if (!supportsGray16Image() || !base || !_old || !ensureLsb()) return false;
  ensureGray16Config();
  // The levels of the page on the glass, for the dissolve. 192 KB: PSRAM, kept
  // (shared with displayGray16Image), as there.
  const PanelGeometry g = geometry();
  const uint32_t levelBytes = static_cast<uint32_t>(g.width / 2) * g.height;
  if (!_gray16) _gray16 = static_cast<uint8_t*>(heap_caps_malloc(levelBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const bool clean = _needsInitialFull || _g16FastTurns >= _g16ScrubEvery;
  const bool dissolve = !clean && _textOnGlass && _gray16;
  _textOnGlass = false;
  // Returns as soon as the refresh starts (_deferRefresh): the caller renders
  // the levels while the panel draws, and finishGray16Text's first upload
  // waits out the waveform. The text is readable when it ends.
  _holdPower = _deferRefresh = true;
  if (dissolve) {
    // After a 16-level text page: one cross-dissolve erases its ink and lays
    // the new page's deep black together (PR #220), so no separate deep pass.
    sendCrossCodes(bus, _gray16, base, _textSoftLevel != 0xFF ? soft : nullptr);
    refreshWith(bus, _lutCross, false);
    memcpy(_old, base, g.bufferSize);  // B/W history for a later stock frame
    _grayPending = false;
  } else {
    // First page, or the clean one every _g16ScrubEvery turns: the stock B/W
    // turn (the previous page's gray driven through _lsb), then a deep pass.
    display(bus, base, nullptr, clean ? RefreshMode::Full : RefreshMode::Fast, false);
  }
  _holdPower = _deferRefresh = false;
  _textDissolved = dissolve;
  _g16FastTurns = clean ? 0 : static_cast<uint8_t>(_g16FastTurns + 1);
  _textBaseUp = true;
  return true;
}

bool Ssd2677Driver::finishGray16Text(EpdBus& bus, const uint8_t* levels, bool turnOff) {
  if (!_textBaseUp) return false;
  _textBaseUp = false;
  if (!levels) {
    if (turnOff) powerOffBetweenFrames(bus);
    return true;
  }
  // The calibrated 16-level passes on top of the readable base: the deep
  // black on the dark levels (black in the base already), then coarse black
  // and the pull-back, as displayGray16Image draws from a clean page.
  settle(bus);  // the base waveform
  if (_g16.useDeep && !_textDissolved) {  // the dissolve laid it already
    sendLevelCodes(bus, levels, _g16.deep);
    refreshWith(bus, _lutDeep, false);
  }
  if (_g16.useCoarse) {
    sendLevelCodes(bus, levels, _g16.coarse);
    refreshWith(bus, _lutCoarse, false);
  }
  uint8_t pull[16];
  memcpy(pull, _g16.pull, 16);
  if (_textDissolved && _textSoftLevel != 0xFF) pull[_textSoftLevel] = 3;  // pulled back in the dissolve
  sendLevelCodes(bus, levels, pull);
  refreshWith(bus, _lutPull, false);
  if (turnOff) {
    // Power off without waiting for the rails to discharge (~230 ms): the
    // page is complete, and the next bus access settles it.
    bus.cmd(CMD_POWER_OFF);
    bus.data(0x00);
    _needsPowerOn = true;
    _refreshInFlight = true;
    _refreshStartMs = millis();
  }
  // B/W history: _old holds the base; every inked level has since moved off
  // it, so flag them in _lsb and the next stock B/W frame drives each to its
  // target, as after the 4-shade text pass.
  const PanelGeometry g = geometry();
  const uint32_t pairs = static_cast<uint32_t>(g.width / 2) * g.height;
  for (uint32_t i = 0, o = 0; i < pairs; i += 4, ++o) {
    uint8_t m = 0;
    for (int k = 0; k < 4; ++k) {
      const uint8_t b = levels[i + k];
      const uint8_t hi = b >> 4, lo = b & 15;
      // Any inked level was driven: the next B/W frame drives it to its target.
      if (hi) m |= static_cast<uint8_t>(0x80 >> (2 * k));
      if (lo) m |= static_cast<uint8_t>(0x40 >> (2 * k));
    }
    _lsb[o] = m;
  }
  _grayPending = true;
  _grayMaskReady = false;
  if (_gray16) {  // the next text turn dissolves from these levels
    memcpy(_gray16, levels, pairs);
    _textOnGlass = true;
  }
  return true;
}

void Ssd2677Driver::sendCrossCodes(EpdBus& bus, const uint8_t* oldLevels, const uint8_t* base, const uint8_t* soft) {
  settle(bus);
  const PanelGeometry g = geometry();
  uint8_t* const chunk = g_txChunk;
  bus.cmd(CMD_DATA);
  auto txn = bus.beginTxn();
  uint16_t n = 0;
  // Four pixels per wire byte: two level bytes, half a base byte.
  const uint32_t bytes = static_cast<uint32_t>(g.width / 2) * g.height;
  for (uint32_t i = 0; i < bytes; i += 2) {
    const uint8_t ink = static_cast<uint8_t>(~base[i / 4]);
    const uint8_t softInk = soft ? static_cast<uint8_t>(~soft[i / 4]) : 0;
    const int shift = (i & 2) ? 0 : 4;  // high nibble of the base byte = first four pixels
    uint8_t out = 0;
    for (int k = 0; k < 4; ++k) {
      const uint8_t lv = oldLevels[i + k / 2];
      const uint8_t old = (k & 1) ? (lv & 15) : (lv >> 4);
      const bool wasInk = old != 0, deep = (ink >> (shift + 3 - k)) & 1;
      const bool isSoft = (softInk >> (shift + 3 - k)) & 1;
      const uint8_t code = deep ? (isSoft ? 0 : 2) : (wasInk ? 1 : 3);
      out = static_cast<uint8_t>(out | code << (6 - 2 * k));
    }
    chunk[n++] = out;
    if (n == sizeof(g_txChunk)) {
      txn.writeBytes(chunk, n);
      n = 0;
    }
  }
  if (n) txn.writeBytes(chunk, n);
}

void Ssd2677Driver::deepSleep(EpdBus& bus) {
  settle(bus);
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
