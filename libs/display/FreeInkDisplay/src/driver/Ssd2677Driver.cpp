#include "Ssd2677Driver.h"

#include <BoardConfig.h>

#if FREEINK_DEVICE_PICCO || FREEINK_DEVICE_STICKY

#include <cstring>

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

// 8 pixels of old + new -> two bytes of 2-bit pixels, old in the high bit,
// MSB-first (stock FUN_422869b4).
inline void packPixels(uint8_t oldByte, uint8_t newByte, uint8_t* out) {
  uint16_t v = 0;
  for (int i = 7; i >= 0; --i) v = static_cast<uint16_t>((v << 2) | (((oldByte >> i) & 1) << 1) | ((newByte >> i) & 1));
  out[0] = static_cast<uint8_t>(v >> 8);
  out[1] = static_cast<uint8_t>(v);
}

}  // namespace

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

  _needsInitialFull = true;
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
  // First paint after init clears the panel with the full waveform (stock forces
  // mode 0 on the first frame).
  if (_needsInitialFull) mode = RefreshMode::Full;
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
  sendPacked(bus, _old, fb);
  // Stock powers off after every refresh (PON, 0x12, POF), whatever `turnOff` says.
  (void)turnOff;
  refreshWith(bus, mode == RefreshMode::Fast ? lut_picco_se0400_fast : lut_picco_se0400_full, true);
  if (_old) memcpy(_old, fb, geometry().bufferSize);  // new becomes old for the next frame
  _needsInitialFull = false;
}

void Ssd2677Driver::sendPacked(EpdBus& bus, const uint8_t* hi, const uint8_t* lo) {
  const PanelGeometry g = geometry();
  static uint8_t chunk[1024];
  // The Sticky panel scans rows bottom-to-top relative to the framebuffer
  // (stock mirrors in software); the Picco takes them in order.
  const bool reversed = isSticky();
  bus.cmd(CMD_DATA);
  auto txn = bus.beginTxn();
  uint16_t n = 0;
  for (uint16_t r = 0; r < g.height; ++r) {
    const uint32_t row = static_cast<uint32_t>(reversed ? g.height - 1 - r : r) * g.widthBytes;
    for (uint16_t xb = 0; xb < g.widthBytes; ++xb) {
      // hi == nullptr → an all-white (0xFF) old plane (stock's begin-time seed).
      packPixels(hi ? hi[row + xb] : 0xFF, lo[row + xb], &chunk[n]);
      n += 2;
      if (n == sizeof(chunk)) {
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

void Ssd2677Driver::powerAndRefresh(EpdBus& bus, bool turnOff) {
  if (_needsPowerOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" SSD2677 power on");
    _needsPowerOn = false;
  }
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

void Ssd2677Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (!lsb) return;
  const uint32_t size = geometry().bufferSize;
  if (!_lsb) _lsb = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (_lsb) memcpy(_lsb, lsb, size);
}

void Ssd2677Driver::beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback,
                                   bool turnOff) {
  _absoluteGray = isSticky() && mode != GrayscaleMode::Overlay;
  if (_absoluteGray) return;  // stock gray4 drives every pixel from any prior state: no base
  displayGrayscaleBase(bus, fb, fallback, turnOff);
}

void Ssd2677Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (!msb || !_lsb) return;
  if (_absoluteGray) {
    // Stock gray4 (Seeed_GFX2 Driver_SSD2677): level index on the wire, 00 black,
    // 01 dark, 10 light, 11 white. Absolute planes: LSB = dark bit, MSB = light
    // bit, so MSB goes high.
    latchOtpWaveform(bus, OTP_GRAY_ROOM);
    sendPacked(bus, msb, _lsb);
    return;
  }
  // Sticky: the B/W base latched an OTP waveform (E0 12 / E6 / A5). Re-run reset
  // + init so the gray LUT runs from the same controller state as on the Picco,
  // which never latches one. Reset leaves the glass as it is.
  if (isSticky()) initPanel(bus);
  // Stock (FUN_42286a70): MSB mask in the high bit, LSB mask in the low bit.
  sendPacked(bus, msb, _lsb);
}

void Ssd2677Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)lut;
  (void)factoryMode;
  if (_absoluteGray) {
    _absoluteGray = false;
    if (!_lsb) {
      display(bus, fb, nullptr, RefreshMode::Full, turnOff);
      return;
    }
    // Stock powers off after gray4. The glass now holds gray a differential
    // refresh can't clear, so the next B/W frame is a full one.
    powerAndRefresh(bus, true);
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
    display(bus, fb, nullptr, RefreshMode::Full, turnOff);
    return;
  }
  refreshWith(bus, lut_picco_se0400_gray, true);
  // NB: do NOT force the next frame to a full refresh here. Every reader page in
  // AA mode is a gray frame, so forcing a full on the following page flashed the
  // whole screen black on every page turn. The retained `_old` still holds the
  // B/W base drawn before this overlay, so the next page diffs against it
  // correctly; CrossPoint schedules its own periodic full refreshes to clean any
  // AA-edge residual (stock does the same — its gray present sets no full flag).
}

void Ssd2677Driver::deepSleep(EpdBus& bus) {
  // Sticky stock powers the rails off first if they are on.
  if (isSticky() && !_needsPowerOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.data(0x00);
    bus.waitBusy(" SSD2677 power off");
  }
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
