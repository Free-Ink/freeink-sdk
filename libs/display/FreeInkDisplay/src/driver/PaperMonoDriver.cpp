#include "PaperMonoDriver.h"

#include <BoardConfig.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace freeink {
namespace {
constexpr uint8_t CMD_SOFT_RESET = 0x12;
constexpr uint8_t CMD_WRITE_NEW = 0x24;
constexpr uint8_t CMD_WRITE_OLD = 0x26;
constexpr uint8_t CMD_SET_RAM_X_RANGE = 0x44;
constexpr uint8_t CMD_SET_RAM_Y_RANGE = 0x45;
constexpr uint8_t CMD_SET_RAM_X_COUNTER = 0x4E;
constexpr uint8_t CMD_SET_RAM_Y_COUNTER = 0x4F;
constexpr uint16_t WIDTH = 800;
constexpr uint16_t HEIGHT = 480;
constexpr uint16_t WIDTH_BYTES = WIDTH / 8;
constexpr uint32_t BUFFER_SIZE = static_cast<uint32_t>(WIDTH_BYTES) * HEIGHT;
constexpr uint16_t ROTATE_CHUNK_BYTES = 16384;

// Display Update Control 2. A custom trigger must omit the OTP LUT and
// temperature reload bits 0x10/0x20 -- either one silently replaces the
// injected waveform with the vendor's. 0xCC brings the analog rails up and
// leaves them up, 0x0C reuses rails that are already live, 0x03 shuts them
// down once ActivityManager proves the controller queue is empty.
constexpr uint8_t CTRL_CUSTOM_HOLD_COLD = 0xCC;
constexpr uint8_t CTRL_DISPLAY_HOLD_WARM = 0x0C;
constexpr uint8_t CTRL_POWER_OFF = 0x03;
// Paper Mono's internal non-flashing B/W waveform, with LUT and temperature
// reload but without the trailing analog/clock power-down. Repeated UI updates
// can then reuse the resident OTP LUT through 0x0C.
constexpr uint8_t CTRL_OTP_BW_HOLD = 0xFC;

// Source driving voltage selectors inside a LUT phase, two bits per sub-phase.
constexpr uint8_t VS_BLACK = 0x01;  // VSH1, +15 V
constexpr uint8_t VS_WHITE = 0x02;  // VSL,  -15 V
constexpr uint8_t VS_WEAK = 0x03;   // VSH2, +5 V

// Frame-rate code 0x08 is 5000 us/frame, the fastest the controller offers.
// Every timing in this file is expressed in those frames.
constexpr uint8_t FRAME_RATE_CODE = 0x08;

// Lab-validated analog set for this glass, written on every custom LUT load
// exactly like the panel lab's loadLut(). The whole tone model -- the 10-frame
// strong-rail swing, the 60-frame weak-rail swing, every balance sum in
// makeTriLut() -- was measured under these values. An earlier build preserved
// the module's OTP analog bank instead; that was never on glass in the lab,
// and shipped with no middle tone (unknown VSH2 made the weak develop do
// nothing, so AA edges stayed white) and a tinted background (unknown
// VSL/VCOM).
constexpr uint8_t VOLT_VGH = 0x17;   // gate high
constexpr uint8_t VOLT_VSH1 = 0x41;  // +15 V strong black rail
constexpr uint8_t VOLT_VSH2 = 0xA8;  // +5 V weak rail, the middle tone's rail
constexpr uint8_t VOLT_VSL = 0x32;   // -15 V strong white rail
constexpr uint8_t VOLT_VCOM = 0x30;  // common electrode DC

// Measured: the strong rail covers the full optical swing in about ten frames.
// A class whose white phase is shorter than this is no longer guaranteed to
// land on the same shade regardless of where it started.
constexpr uint16_t SAT_MIN_FRAMES = 12;
// Per-page top-up for unchanged white background (LUT entry 0): one +15 V
// frame to unstick settled pigment, then the white dose. The 1:5 ratio is a
// deliberate white-biased imbalance (net -60 V*frames/page): background ghost
// residue is always dark, so the safe drift direction is toward white, and the
// user's corrective cadence rebalances the driven classes anyway. Both pulses
// ride inside the activation kick, so the top-up costs zero extra frames, and
// at 5 ms a single 1-frame excursion is below the pigment's optical response.
constexpr uint8_t BG_TOPUP_BLACK = 1;
constexpr uint8_t BG_TOPUP_WHITE = 5;
// Extra same-direction drive passes each boot paint runs on top of its normal
// activation (see runBootCleanPass()). Repetition demonstrably fades residue
// on this glass; two extras triple the dose per boot paint at ~300 ms each.
constexpr uint8_t BOOT_CLEAN_EXTRA_PASSES = 2;
// Paper Mono's 180-degree mount requires a reversed staging buffer. Keep one
// controller-worker-owned block in internal RAM: larger bursts cut the Arduino
// SPI transaction setup count from 94 to 3 per plane without consuming the
// render task's 8 KB stack.
DRAM_ATTR uint8_t ROTATE_CHUNK[ROTATE_CHUNK_BYTES];

constexpr std::array<uint8_t, 256> makeReverseBitsLut() {
  std::array<uint8_t, 256> lut{};
  for (uint16_t input = 0; input < 256; ++input) {
    uint8_t value = static_cast<uint8_t>(input);
    value = static_cast<uint8_t>(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
    value = static_cast<uint8_t>(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
    lut[input] = static_cast<uint8_t>((value << 4) | (value >> 4));
  }
  return lut;
}
constexpr auto REVERSE_BITS_LUT = makeReverseBitsLut();

// Xtensa has no population-count instruction, so __builtin_popcount lowers to a
// windowed callx8 into ROM's __popcountsi2 (verified in the linked ELF). The
// plane-composition loops below run it once per framebuffer byte, which is tens
// of thousands of function calls per page turn purely to feed a serial-log
// statistic. This SWAR form inlines to a handful of register ops and needs no
// table, so the counters stay exact and cost effectively nothing.
constexpr uint8_t popcount8(uint8_t value) {
  value = static_cast<uint8_t>(value - ((value >> 1) & 0x55u));
  value = static_cast<uint8_t>((value & 0x33u) + ((value >> 2) & 0x33u));
  return static_cast<uint8_t>((value + (value >> 4)) & 0x0Fu);
}

// The SSD1677 waveform block. Bytes 0..104 go out via cmd 0x32; the analog
// tail 105..109 goes out as the VOLT_* register writes in loadCustomLut():
//   [0..49]    five LUT entries x ten groups; each byte is four sub-phases at
//              two bits, phase A in the most significant pair
//   [50..99]   ten groups x {TP_A, TP_B, TP_C, TP_D, RP}; a group runs RP+1
//              times, so RP counts *extra* repeats
//   [100..104] frame-rate nibbles, two groups per byte
//   [105..109] VGH, VSH1, VSH2, VSL, VCOM (registers 0x03/0x04/0x2C)
struct WaveLut {
  uint8_t b[111];

  void clear() { memset(b, 0, sizeof(b)); }

  void setVs(uint8_t entry, uint8_t group, uint8_t phase, uint8_t vs) {
    uint8_t& target = b[entry * 10 + group];
    const uint8_t shift = static_cast<uint8_t>((3 - phase) * 2);
    target = static_cast<uint8_t>((target & ~(0x03u << shift)) | ((vs & 0x03u) << shift));
  }

  void setTp(uint8_t group, uint8_t a, uint8_t bb, uint8_t c, uint8_t d, uint8_t rp) {
    b[50 + group * 5 + 0] = a;
    b[50 + group * 5 + 1] = bb;
    b[50 + group * 5 + 2] = c;
    b[50 + group * 5 + 3] = d;
    b[50 + group * 5 + 4] = rp;
  }

  void finish() {
    const uint8_t packed = static_cast<uint8_t>((FRAME_RATE_CODE & 0x0F) | ((FRAME_RATE_CODE & 0x0F) << 4));
    for (uint8_t i = 0; i < 5; ++i) b[100 + i] = packed;
  }
};

void sortAscending(uint16_t* values, uint8_t count) {
  for (uint8_t i = 1; i < count; ++i) {
    for (uint8_t j = i; j > 0 && values[j] < values[j - 1]; --j) {
      const uint16_t swap = values[j];
      values[j] = values[j - 1];
      values[j - 1] = swap;
    }
  }
}
}  // namespace

PaperMonoDriver& paperMonoDriver() {
  static PaperMonoDriver driver;
  return driver;
}

void paperMonoSetGrayParams(const PaperMonoGrayParams& params) { paperMonoDriver().setGrayParams(params); }
void paperMonoAbortGray() { paperMonoDriver().abortGray(); }
void paperMonoResetGray() { paperMonoDriver().resetGray(); }

uint32_t PaperMonoDriver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 20000000;
}

PanelGeometry PaperMonoDriver::geometry() const { return {WIDTH, HEIGHT, WIDTH_BYTES, BUFFER_SIZE}; }

bool PaperMonoDriver::allocateBuffers() {
  const auto alloc = [](uint8_t*& ptr, size_t size) {
    if (!ptr) ptr = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ptr != nullptr;
  };
  const bool glassWasMissing = _glassNonWhite == nullptr || _glassBlack == nullptr;
  const bool ok = alloc(_lastBw, BUFFER_SIZE) && alloc(_pendingBw, BUFFER_SIZE) && alloc(_grayLsb, BUFFER_SIZE) &&
                  alloc(_grayMsb, BUFFER_SIZE) && alloc(_glassNonWhite, BUFFER_SIZE) &&
                  alloc(_glassBlack, BUFFER_SIZE) && alloc(_sel24, BUFFER_SIZE) && alloc(_sel26, BUFFER_SIZE);
  // An unknown glass state must never be diffed against. Start it at "all
  // white" and let _needsFull force the first waveform to drive every pixel.
  if (glassWasMissing && ok) {
    memset(_glassNonWhite, 0, BUFFER_SIZE);
    memset(_glassBlack, 0, BUFFER_SIZE);
  }
  return ok;
}

void PaperMonoDriver::begin(EpdBus& bus) {
  allocateBuffers();
  bus.reset();
  initController(bus);
  _needsFull = true;
  // The first paints after boot (splash, then whatever replaces it — the one
  // that must erase the dwelled logo) each get extra drive passes so pre-boot
  // and splash residue doesn't ghost through (see runBootCleanPass()). Three
  // paints so an intermediate progress paint can't exhaust the budget before
  // the home screen lands.
  _bootCleanPaints = 3;
  _lastBwValid = false;
  _abortGeneration.store(0);
  _displayWorkGeneration = 0;
  _controllerPowered = false;
  _lutState = LutState::Unknown;
  _windowBaselineValid = false;
  resetGray();
}

void PaperMonoDriver::initController(EpdBus& bus) {
  bus.cmd(CMD_SOFT_RESET);
  bus.waitBusy("PaperMono reset");
  _controllerPowered = false;
  _lutState = LutState::Unknown;
  _windowBaselineValid = false;

  bus.cmd(0x18);
  bus.data(0x80);
  const uint8_t softStart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
  bus.cmdData(0x0C, softStart, sizeof(softStart));

  bus.cmd(0x01);
  bus.data(static_cast<uint8_t>((HEIGHT - 1) & 0xFF));
  bus.data(static_cast<uint8_t>((HEIGHT - 1) >> 8));
  bus.data(0x02);
  // Keep the border on VCOM, as used by the panel-lab characterisation. On the
  // SSD1677 A[7:6]=10 means VCOM; actual HiZ would be 0xC0.
  bus.cmd(0x3C);
  bus.data(0x80);
  bus.cmd(0x11);
  // Keep the controller in Paper Mono's validated X-/Y+ scan layout. The panel
  // mount needs this controller-side X direction in addition to writePlane()'s
  // raster transform; X+/Y+ adds a visible left/right mirror on this hardware.
  bus.data(0x02);

  bus.cmd(CMD_SET_RAM_X_RANGE);
  const uint16_t xStart = WIDTH - 1;
  const uint16_t xEnd = 0;
  bus.data(static_cast<uint8_t>(xStart & 0xFF));
  bus.data(static_cast<uint8_t>(xStart >> 8));
  bus.data(static_cast<uint8_t>(xEnd & 0xFF));
  bus.data(static_cast<uint8_t>(xEnd >> 8));
  bus.cmd(CMD_SET_RAM_Y_RANGE);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(static_cast<uint8_t>((HEIGHT - 1) & 0xFF));
  bus.data(static_cast<uint8_t>((HEIGHT - 1) >> 8));

  // Disable RAM inverse/bypass so the selector planes reach the LUT verbatim.
  bus.cmd(0x21);
  bus.data(0x00);

  resetRamCounter(bus);
  _initialized = true;
}

void PaperMonoDriver::setRamWindow(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  // Paper Mono uses X-/Y+ data entry. Reversing each source row already
  // aligns framebuffer X with controller X; mirroring the RAM X range again
  // moves the driven rectangle to WIDTH-x-w. Only Y needs the mount flip.
  const uint16_t xStart = static_cast<uint16_t>(x + w - 1);
  const uint16_t xEnd = x;
  const uint16_t yStart = static_cast<uint16_t>(HEIGHT - y - h);
  const uint16_t yEnd = static_cast<uint16_t>(HEIGHT - 1 - y);

  bus.cmd(CMD_SET_RAM_X_RANGE);
  bus.data(static_cast<uint8_t>(xStart & 0xFF));
  bus.data(static_cast<uint8_t>(xStart >> 8));
  bus.data(static_cast<uint8_t>(xEnd & 0xFF));
  bus.data(static_cast<uint8_t>(xEnd >> 8));
  bus.cmd(CMD_SET_RAM_Y_RANGE);
  bus.data(static_cast<uint8_t>(yStart & 0xFF));
  bus.data(static_cast<uint8_t>(yStart >> 8));
  bus.data(static_cast<uint8_t>(yEnd & 0xFF));
  bus.data(static_cast<uint8_t>(yEnd >> 8));
  bus.cmd(CMD_SET_RAM_X_COUNTER);
  bus.data(static_cast<uint8_t>(xStart & 0xFF));
  bus.data(static_cast<uint8_t>(xStart >> 8));
  bus.cmd(CMD_SET_RAM_Y_COUNTER);
  bus.data(static_cast<uint8_t>(yStart & 0xFF));
  bus.data(static_cast<uint8_t>(yStart >> 8));
}

void PaperMonoDriver::restoreFullRamWindow(EpdBus& bus) { setRamWindow(bus, 0, 0, WIDTH, HEIGHT); }

void PaperMonoDriver::resetRamCounter(EpdBus& bus) {
  const uint16_t xStart = WIDTH - 1;
  bus.cmd(CMD_SET_RAM_X_COUNTER);
  bus.data(static_cast<uint8_t>(xStart & 0xFF));
  bus.data(static_cast<uint8_t>(xStart >> 8));
  bus.cmd(CMD_SET_RAM_Y_COUNTER);
  bus.data(0x00);
  bus.data(0x00);
}

void PaperMonoDriver::writePlane(EpdBus& bus, uint8_t command, const uint8_t* data) {
  if (!data) return;
  resetRamCounter(bus);
  bus.cmd(command);
  const bool rotate180 = BoardConfig::ACTIVE.orientation.mirrorX && BoardConfig::ACTIVE.orientation.mirrorY;
  if (!rotate180) {
    bus.data(data, static_cast<uint16_t>(BUFFER_SIZE));
    return;
  }

  // A 180-degree 1-bpp raster rotation is a byte-order reversal plus a bit
  // reversal inside every byte. Stream it in bounded chunks so no second
  // framebuffer is required and keep CS asserted for the whole RAM write.
  bus.beginTxn();
  for (uint32_t sent = 0; sent < BUFFER_SIZE; sent += ROTATE_CHUNK_BYTES) {
    const uint16_t count =
        static_cast<uint16_t>((BUFFER_SIZE - sent) < ROTATE_CHUNK_BYTES ? (BUFFER_SIZE - sent) : ROTATE_CHUNK_BYTES);
    for (uint16_t i = 0; i < count; ++i) ROTATE_CHUNK[i] = REVERSE_BITS_LUT[data[BUFFER_SIZE - 1 - sent - i]];
    bus.rawWriteBytes(ROTATE_CHUNK, count);
  }
  bus.endTxn();
}

void PaperMonoDriver::writePlaneWindow(EpdBus& bus, uint8_t command, const uint8_t* data, uint16_t x, uint16_t y,
                                       uint16_t w, uint16_t h) {
  if (!data) return;
  setRamWindow(bus, x, y, w, h);
  const uint16_t firstByte = x / 8;
  const uint16_t widthBytes = w / 8;
  uint16_t staged = 0;

  bus.cmd(command);
  bus.beginTxn();
  for (uint16_t rowOffset = 0; rowOffset < h; ++rowOffset) {
    const uint16_t sourceY = static_cast<uint16_t>(y + h - 1 - rowOffset);
    const uint32_t rowBase = static_cast<uint32_t>(sourceY) * WIDTH_BYTES;
    for (uint16_t byteOffset = 0; byteOffset < widthBytes; ++byteOffset) {
      const uint16_t sourceByte = static_cast<uint16_t>(firstByte + widthBytes - 1 - byteOffset);
      ROTATE_CHUNK[staged++] = REVERSE_BITS_LUT[data[rowBase + sourceByte]];
      if (staged == ROTATE_CHUNK_BYTES) {
        bus.rawWriteBytes(ROTATE_CHUNK, staged);
        staged = 0;
      }
    }
  }
  if (staged > 0) bus.rawWriteBytes(ROTATE_CHUNK, staged);
  bus.endTxn();
}

void PaperMonoDriver::activate(EpdBus& bus, uint8_t control) {
  // An activation may swap or consume the controller plane roles. A future
  // window update must not trust them until they are explicitly re-seeded.
  _windowBaselineValid = false;
  bus.cmd(0x22);
  bus.data(control);
  bus.cmd(0x20);
  bus.waitRefreshComplete("PaperMono refresh");
}

void PaperMonoDriver::activateOtp(EpdBus& bus) {
  const bool warm = _controllerPowered && _lutState == LutState::OtpBw;
  activate(bus, warm ? CTRL_DISPLAY_HOLD_WARM : CTRL_OTP_BW_HOLD);
  _controllerPowered = true;
  _lutState = LutState::OtpBw;
}

void PaperMonoDriver::runBootCleanPass(EpdBus& bus, const uint8_t* newPlane, const uint8_t* oldPlane) {
  // First paints after a cold boot: the previous firmware image (boot logo,
  // whatever was on the glass) dwelled for seconds, and one non-flashing pass
  // under-erases that residue. Run the same drive a second time so every
  // driven pixel gets a double dose toward its target. No flash: each pass
  // pushes directly toward the target, never through the opposite endpoint.
  // Costs one extra activation (~300 ms) on the boot paints only.
  //
  // The planes MUST be rewritten before each re-trigger: the controller does
  // not preserve plane roles across an activation (re-activating on the
  // "resident" planes came out inverted on glass), which is also why every
  // ordinary update writes both planes fresh.
  if (_bootCleanPaints == 0) return;
  --_bootCleanPaints;
  for (uint8_t pass = 0; pass < BOOT_CLEAN_EXTRA_PASSES; ++pass) {
    writePlane(bus, CMD_WRITE_NEW, newPlane);
    writePlane(bus, CMD_WRITE_OLD, oldPlane);
    activate(bus, CTRL_DISPLAY_HOLD_WARM);
  }
}

void PaperMonoDriver::powerOffController(EpdBus& bus) {
  if (!_controllerPowered) return;
  activate(bus, CTRL_POWER_OFF);
  _controllerPowered = false;
}

void PaperMonoDriver::loadCustomLut(EpdBus& bus, const uint8_t lut[111]) {
  bus.cmd(0x32);
  bus.data(lut, 105);
  // The analog registers are volatile across reset/deep-sleep, and the OTP
  // paths reload their own WS-bank values on every 0xFC trigger, so the custom
  // set must be re-asserted with the LUT every time.
  bus.cmd(0x03);
  bus.data(VOLT_VGH);
  static constexpr uint8_t source[3] = {VOLT_VSH1, VOLT_VSH2, VOLT_VSL};
  bus.cmdData(0x04, source, sizeof(source));
  bus.cmd(0x2C);
  bus.data(VOLT_VCOM);
  _lutState = LutState::Custom;
}

// Endpoint polish. On ordinary pages entry 0 is unchanged white and entry 1 is
// changed-to-white; both therefore receive the same white schedule. Gray and
// black stay on entries 2 and 3. Every entry receives the same number of VSH1
// and VSL frames, so the source impulse remains class-independent. Phase order
// is the only difference: white alternates to avoid a visible dark hold, gray
// makes a closed local excursion, and black groups its release before one
// continuous black settle instead of bleaching the endpoint on every repeat.
uint16_t PaperMonoDriver::makePostCleanLut(uint8_t out[111]) const {
  WaveLut lut;
  lut.clear();
  if (_tri.postCleanCycles == 0) {
    lut.finish();
    memcpy(out, lut.b, 111);
    return 0;
  }

  const uint8_t groups = std::min<uint8_t>(_tri.postCleanCycles, 10);
  const uint16_t totalFrames = static_cast<uint16_t>(4u * groups);
  for (uint8_t group = 0; group < groups; ++group) {
    for (uint8_t phase = 0; phase < 4; ++phase) {
      const uint16_t frame = static_cast<uint16_t>(4u * group + phase);
      const uint8_t whiteVs = (frame & 1u) == 0 ? VS_BLACK : VS_WHITE;
      const uint8_t grayVs = (phase == 0 || phase == 3) ? VS_BLACK : VS_WHITE;
      const uint8_t blackVs = frame < totalFrames / 2 ? VS_WHITE : VS_BLACK;
      lut.setVs(0, group, phase, whiteVs);
      lut.setVs(1, group, phase, whiteVs);
      lut.setVs(2, group, phase, grayVs);
      lut.setVs(3, group, phase, blackVs);
    }
    lut.setTp(group, 1, 1, 1, 1, 0);
  }

  lut.finish();
  memcpy(out, lut.b, 111);
  return totalFrames;
}

// The one-shot three-level waveform used for every Balanced page. G0 is the
// activation kick: the driven classes charge toward their anti-target rail
// (white and gray toward black, black toward white) while unchanged white
// background on entry 0 receives its 1+5 top-up in the same frames. The
// remaining per-class trajectories are right-aligned so white, gray and black
// all finish on their target rail; a left-aligned layout made endpoints look
// gray because they idled at VSS after finishing early.
//
// Gray charges the FULL kick toward black (it used to split the kick into a
// self-canceling black/white pair). That changes its balance equation from
// grayW = tGray/3, which forced tGray >= 36 and an ~80%-black "gray" that
// reads as black on thin CJK strokes, to:
//
//   white: +15*k - 15*whiteW              = 0  ->  whiteW = k
//   gray:  +15*k - 15*grayW  +  5*tGray   = 0  ->  grayW  = k + tGray/3
//   black: -15*k - 15*blackW + 15*tBlack  = 0  ->  blackW = tBlack - k
//
// With the defaults (k=16, tGray=24, tBlack=32) the post-kick trajectories are
// W=16, G=24+24 and B=16+32 frames, right-aligned in a 48-frame tail: 64
// frames / 320 ms total, every class exactly DC balanced, and the middle tone
// lands at ~40% of the weak-rail swing -- clearly separated from both
// endpoints instead of the previous near-black.
uint16_t PaperMonoDriver::makeTriLut(uint8_t out[111], bool bgTopUp) const {
  WaveLut lut;
  lut.clear();

  const uint8_t kick = _tri.preUp;
  const uint16_t whiteFor[3] = {
      kick,
      static_cast<uint16_t>(kick + _tri.tGray / 3),
      static_cast<uint16_t>(_tri.tBlack > kick ? _tri.tBlack - kick : 0),
  };
  uint8_t group = 0;
  uint16_t frames = 0;

  if (kick > 0) {
    const bool topUp = kick >= BG_TOPUP_BLACK + BG_TOPUP_WHITE + 1;
    const uint8_t tpA = topUp ? BG_TOPUP_BLACK : kick;
    const uint8_t tpB = topUp ? BG_TOPUP_WHITE : 0;
    const uint8_t tpC = topUp ? static_cast<uint8_t>(kick - BG_TOPUP_BLACK - BG_TOPUP_WHITE) : 0;
    const uint8_t tp[3] = {tpA, tpB, tpC};
    for (uint8_t phase = 0; phase < 3; ++phase) {
      if (tp[phase] == 0) continue;
      lut.setVs(1, group, phase, VS_BLACK);
      lut.setVs(2, group, phase, VS_BLACK);
      lut.setVs(3, group, phase, VS_WHITE);
    }
    // Overlay passes leave entry 0 fully idle: there, entry 0 holds every
    // undriven pixel — including unchanged BLACK text, not just background —
    // and the white-biased top-up would visibly bleach it each page.
    if (topUp && bgTopUp) {
      lut.setVs(0, group, 0, VS_BLACK);
      lut.setVs(0, group, 1, VS_WHITE);
    }
    lut.setTp(group, tpA, tpB, tpC, 0, 0);
    frames = static_cast<uint16_t>(frames + kick);
    ++group;
  }

  const uint16_t trajectory[3] = {
      whiteFor[0],
      static_cast<uint16_t>(whiteFor[1] + _tri.tGray),
      static_cast<uint16_t>(whiteFor[2] + _tri.tBlack),
  };
  const uint16_t tail = std::max(trajectory[0], std::max(trajectory[1], trajectory[2]));
  const uint16_t start[3] = {
      static_cast<uint16_t>(tail - trajectory[0]),
      static_cast<uint16_t>(tail - trajectory[1]),
      static_cast<uint16_t>(tail - trajectory[2]),
  };

  // Every boundary at which a class changes voltage. Between two consecutive
  // boundaries all entries have a constant source selection and fit one group.
  uint16_t bounds[7] = {
      0,
      start[0],
      start[1],
      static_cast<uint16_t>(start[1] + whiteFor[1]),
      start[2],
      static_cast<uint16_t>(start[2] + whiteFor[2]),
      tail,
  };
  sortAscending(bounds, 7);
  uint16_t previous = 0;
  for (uint8_t i = 0; i < 7 && group < 10; ++i) {
    const uint16_t current = bounds[i];
    if (current <= previous) continue;
    const uint8_t length = static_cast<uint8_t>(current - previous);

    if (previous >= start[0]) lut.setVs(1, group, 0, VS_WHITE);

    if (previous >= start[1]) {
      const uint16_t grayDevelop = static_cast<uint16_t>(start[1] + whiteFor[1]);
      lut.setVs(2, group, 0, previous < grayDevelop ? VS_WHITE : VS_WEAK);
    }

    if (previous >= start[2]) {
      const uint16_t blackDevelop = static_cast<uint16_t>(start[2] + whiteFor[2]);
      lut.setVs(3, group, 0, previous < blackDevelop ? VS_WHITE : VS_BLACK);
    }

    lut.setTp(group, length, 0, 0, 0, 0);
    frames = static_cast<uint16_t>(frames + length);
    previous = current;
    ++group;
  }

  lut.finish();
  memcpy(out, lut.b, 111);
  return frames;
}

// Binary UI and the reader's Fast mode stay on the panel's own temperature-
// selected, non-flashing OTP waveform. The host glass model is still used to
// synthesize the OLD plane: a physical gray has no exact binary OLD value, so
// force it to the endpoint opposite the target and make the OTP transition
// definite. `forceAll` does the same for every pixel when the glass history is
// unknown or the caller explicitly requests a full resync.
bool PaperMonoDriver::runOtpUpdate(EpdBus& bus, const uint8_t* bwTarget, bool forceAll) {
  if (!bwTarget || !allocateBuffers()) return false;
  if (!_initialized) {
    bus.reset();
    initController(bus);
  }

  // Only "did anything change at all" gates the update; the exact bit count is
  // a log statistic. __builtin_popcount does not inline on Xtensa — it compiles
  // to a windowed callx8 into ROM's __popcountsi2 — so accumulating it per byte
  // cost one function call per byte of the framebuffer on every refresh. OR the
  // masks together instead, and pay for popcount only when a log will print it.
  uint8_t changedBits = 0;
  // Inverted content: the unchanged black background is where every
  // white->black transition's light residue parks, and with no flashing
  // corrective on this panel it accumulates without bound. Present those
  // pixels as old-white so the OTP waveform runs its full white->black drive
  // and re-blackens the background on every update — the drive is optically
  // invisible on an already-black pixel. This mirrors what the tri path
  // already does (it drives every non-white pixel per page). The repeated
  // black-going impulse on static background is a deliberate DC imbalance in
  // the safe direction for a dark background (residue there is always light);
  // the user's corrective cadence rebalances, as with the entry-0 top-up.
  const uint8_t darkDrive = _darkBackground ? 0xFFu : 0x00u;
#ifdef ENABLE_SERIAL_LOG
  uint32_t changed = 0;
#endif
  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    const uint8_t bw = bwTarget[i];  // set bit = white
    if (forceAll) {
      _sel26[i] = static_cast<uint8_t>(~bw);
      changedBits = 0xFF;
#ifdef ENABLE_SERIAL_LOG
      changed += 8;
#endif
      continue;
    }

    const uint8_t targetBlack = static_cast<uint8_t>(~bw);
    const uint8_t oldNonWhite = _glassNonWhite[i];
    const uint8_t oldBlack = _glassBlack[i];
    const uint8_t oldGray = static_cast<uint8_t>(oldNonWhite & ~oldBlack);
    const uint8_t changedMask =
        static_cast<uint8_t>((oldNonWhite ^ targetBlack) | (oldBlack ^ targetBlack));
    changedBits |= changedMask;
#ifdef ENABLE_SERIAL_LOG
    changed += popcount8(changedMask);
#endif

    // SSD1677 OTP B/W: one means old white, zero means old black. For gray,
    // choose the opposite of the target so the controller cannot classify it
    // as an unchanged endpoint.
    const uint8_t oldWhite = static_cast<uint8_t>(~oldNonWhite);
    _sel26[i] = static_cast<uint8_t>(oldWhite | (targetBlack & oldGray) | (targetBlack & darkDrive));
  }
  if (changedBits == 0) return false;

  const unsigned long started = millis();
  bus.cmd(0x3C);
  bus.data(0x80);  // VCOM border; do not let the custom LUT drive a dark rim.
  writePlane(bus, CMD_WRITE_NEW, bwTarget);
  writePlane(bus, CMD_WRITE_OLD, _sel26);
  activateOtp(bus);
  runBootCleanPass(bus, bwTarget, _sel26);

  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    const uint8_t black = static_cast<uint8_t>(~bwTarget[i]);
    _glassNonWhite[i] = black;
    _glassBlack[i] = black;
  }
  _panelHasGray = false;

#ifdef ENABLE_SERIAL_LOG
  Serial.printf("[%lu] SSD1677 OTP B/W: changed=%lu forced=%u elapsed=%lums\n", millis(),
                static_cast<unsigned long>(changed), static_cast<unsigned>(forceAll), millis() - started);
#else
  (void)started;
#endif
  return true;
}

bool PaperMonoDriver::runUpdate(EpdBus& bus, const uint8_t* bwTarget, bool useGray, bool corrective,
                                bool overlayOnly) {
  if (!useGray) {
    const bool otpRan = runOtpUpdate(bus, bwTarget, corrective);
    if (otpRan) _displayCommitted = true;
    return otpRan;
  }
  if (!bwTarget || !allocateBuffers()) return false;
  if (!_initialized) {
    bus.reset();
    initController(bus);
  }

  // q24 means non-white and q26 means black, so level = q24 + q26 gives
  // W=0, G=1, B=2. A corrective update maps every pixel to target-coded entry
  // W=1, G=2, B=3. On an ordinary page, unchanged white maps to entry 0 while
  // changed pixels and all target gray/black pixels remain actively driven.
  // This removes the dominant full-background flash without letting stable
  // text fade. Stage 2 explicitly treats both entries 0 and 1 as white.
  // See runOtpUpdate(): popcount is a ROM call on Xtensa, and both counters are
  // log-only — `changed` is otherwise tested just for zero, `driven` is never
  // read at all. Keeping them out of the release build removes two calls per
  // framebuffer byte from the hottest loop in a gray page turn.
  uint8_t changedBits = 0;
#ifdef ENABLE_SERIAL_LOG
  uint32_t changed = 0;
  uint32_t driven = 0;
#endif
  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    const uint8_t bw = bwTarget[i];  // set bit = white
    const uint8_t gray = useGray ? static_cast<uint8_t>(_grayLsb[i] | _grayMsb[i]) : 0u;
    const uint8_t q24 = static_cast<uint8_t>(gray | ~bw);
    const uint8_t q26 = static_cast<uint8_t>(~bw & ~gray);
    const uint8_t old24 = _glassNonWhite[i];
    const uint8_t old26 = _glassBlack[i];
    const uint8_t changedMask = static_cast<uint8_t>((old24 ^ q24) | (old26 ^ q26));
    // Overlay: the B/W base already reached the glass through its own OTP
    // activation (the host displayed it before staging gray planes), so only
    // the pixels whose class actually changes — the AA grays — may be driven.
    // Re-driving the whole non-white body here is what reads as a full-screen
    // flash. The combined single-activation path keeps `| q24` because there
    // the tri waveform is the only drive the page gets.
    const uint8_t driveMask =
        corrective ? 0xFFu : (overlayOnly ? changedMask : static_cast<uint8_t>(changedMask | q24));
    changedBits |= corrective ? 0xFFu : changedMask;
#ifdef ENABLE_SERIAL_LOG
    changed += corrective ? 8u
                          : popcount8(changedMask);
    driven += popcount8(driveMask);
#endif
    _sel24[i] = static_cast<uint8_t>(driveMask & ~(q24 ^ q26));
    _sel26[i] = static_cast<uint8_t>(driveMask & q24);
  }
  if (changedBits == 0) return false;

  const unsigned long started = millis();
  uint8_t lut[111];
  const uint16_t firstFrames = makeTriLut(lut, /*bgTopUp=*/!overlayOnly);
  uint16_t cleanFrames = 0;
  uint8_t stages = 1;
  uint32_t cleanedPixels = 0;

  writePlane(bus, CMD_WRITE_NEW, _sel24);
  writePlane(bus, CMD_WRITE_OLD, _sel26);
  loadCustomLut(bus, lut);
  activate(bus, _controllerPowered ? CTRL_DISPLAY_HOLD_WARM : CTRL_CUSTOM_HOLD_COLD);
  _controllerPowered = true;
  // No boot-clean pass here: re-running the tri LUT is not direction-safe the
  // way the OTP B/W waveform is. Its activation kick charges driven pixels
  // toward the anti-target rail and entry 0 hits the whole white background
  // with the +15 V top-up, so an extra pass reads as a full-screen flash on
  // every AA page inside the boot budget. Residue cleanup stays on the OTP
  // path only (runOtpUpdate), which the boot paints go through anyway.

  // Retired in production (postCleanCycles is forced to 0): background deghost
  // now rides inside the tri activation's kick group, and the right-aligned
  // classes already end target-directed. Kept for lab experiments only.
  if (_tri.postCleanCycles > 0) {
    cleanFrames = makePostCleanLut(lut);
    loadCustomLut(bus, lut);
    activate(bus, CTRL_DISPLAY_HOLD_WARM);
    ++stages;
    cleanedPixels = static_cast<uint32_t>(WIDTH) * HEIGHT;
  }

  // Commit the recorded glass state only after every required activation has
  // completed. Advancing it before the controller write made a failed update
  // look successful and poisoned every subsequent transition classification.
  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    const uint8_t bw = bwTarget[i];
    const uint8_t gray = useGray ? static_cast<uint8_t>(_grayLsb[i] | _grayMsb[i]) : 0u;
    _glassNonWhite[i] = static_cast<uint8_t>(gray | ~bw);
    _glassBlack[i] = static_cast<uint8_t>(~bw & ~gray);
  }
  _panelHasGray = useGray && (_grayLsbReady || _grayMsbReady);

#ifdef ENABLE_SERIAL_LOG
  Serial.printf("[%lu] SSD1677 update: stages=%u frames=%u+%u changed=%lu drive=%lu clean=%lu gray=%u full=%u "
                "elapsed=%lums\n",
                millis(), static_cast<unsigned>(stages), static_cast<unsigned>(firstFrames),
                static_cast<unsigned>(cleanFrames), static_cast<unsigned long>(changed),
                static_cast<unsigned long>(driven),
                static_cast<unsigned long>(cleanedPixels),
                static_cast<unsigned>(useGray), static_cast<unsigned>(corrective), millis() - started);
#else
  (void)firstFrames;
  (void)cleanFrames;
  (void)stages;
  (void)started;
#endif
  _displayCommitted = true;
  return true;
}

void PaperMonoDriver::stashTarget(const uint8_t* fb, RefreshMode mode) {
  if (!allocateBuffers()) return;
  memcpy(_pendingBw, fb, BUFFER_SIZE);
  _pendingTri = true;
  _pendingCorrective = _needsFull || mode == RefreshMode::Full;
  _pendingGeneration = _renderGeneration;
}

bool PaperMonoDriver::commitPending(EpdBus& bus, bool useGray) {
  if (!_pendingTri) return false;
  if (_pendingGeneration != _renderGeneration) {
    _pendingTri = false;
    _pendingCorrective = false;
    clearGrayStaging();
    return false;
  }
  _pendingTri = false;
  const bool corrective = _pendingCorrective;
  const bool ran = runUpdate(bus, _pendingBw, useGray, corrective);
  if (ran) _needsFull = false;
  _pendingCorrective = false;
  if (_lastBw && (ran || !corrective)) {
    memcpy(_lastBw, _pendingBw, BUFFER_SIZE);
    _lastBwValid = true;
  }
  if (!useGray) _panelHasGray = false;
  clearGrayStaging();
  return ran;
}

void PaperMonoDriver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;
  (void)turnOff;
  if (!fb) return;
  // Not initialised no longer implies an unknown glass state: controllerIdle()
  // parks the controller in deep sleep between pages. The paths that really do
  // invalidate the image -- begin(), deepSleep(), requestResync() -- raise
  // _needsFull themselves.
  if (!_initialized) {
    bus.reset();
    initController(bus);
  }
  if (!allocateBuffers()) return;

  const bool corrective = _needsFull || mode == RefreshMode::Full;
  if (!corrective && _lastBwValid && !_panelHasGray && !_grayLsbReady && !_grayMsbReady &&
      memcmp(_lastBw, fb, BUFFER_SIZE) == 0) {
    // UI screens deliberately re-render an identical frame once their async
    // data lands. Submit no waveform, and leave any running deghost alone.
    return;
  }

  stashTarget(fb, mode);
  // displayGrayscaleBase() is the front half of a grayscale sequence: hold the
  // target so the gray planes can join it in a single activation.
  if (!_preparingGray) commitPending(bus, false);
}

void PaperMonoDriver::displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y,
                                    uint16_t w, uint16_t h, bool turnOff) {
  (void)prev;
  if (!fb || w == 0 || h == 0 || x + w > WIDTH || y + h > HEIGHT || x % 8 != 0 || w % 8 != 0) return;
  if (!_initialized) {
    bus.reset();
    initController(bus);
  }
  if (!allocateBuffers()) return;

  const bool rotate180 = BoardConfig::ACTIVE.orientation.mirrorX && BoardConfig::ACTIVE.orientation.mirrorY;
  if (!rotate180 || _needsFull || !_lastBwValid || _panelHasGray || _grayLsbReady || _grayMsbReady) {
    display(bus, fb, prev, RefreshMode::Fast, turnOff);
    return;
  }

  const uint16_t firstByte = x / 8;
  const uint16_t widthBytes = w / 8;
  bool changed = false;
  for (uint16_t row = 0; row < h && !changed; ++row) {
    const uint32_t offset = static_cast<uint32_t>(y + row) * WIDTH_BYTES + firstByte;
    changed = memcmp(fb + offset, _lastBw + offset, widthBytes) != 0;
  }
  if (!changed) return;

  // A normal Paper Mono activation does not preserve trustworthy NEW/OLD
  // plane roles. Seed both full planes once before the first window update;
  // subsequent window activations re-seed just their rectangle below.
  if (!_windowBaselineValid) {
    restoreFullRamWindow(bus);
    writePlane(bus, CMD_WRITE_NEW, _lastBw);
    writePlane(bus, CMD_WRITE_OLD, _lastBw);
    _windowBaselineValid = true;
  }

  writePlaneWindow(bus, CMD_WRITE_NEW, fb, x, y, w, h);
  writePlaneWindow(bus, CMD_WRITE_OLD, _lastBw, x, y, w, h);
  activateOtp(bus);

  // Commit only the addressed rectangle to the host glass model and previous
  // target. Window-external pixels were represented by equal NEW/OLD planes
  // and therefore remained idle during the OTP differential waveform.
  for (uint16_t row = 0; row < h; ++row) {
    const uint32_t offset = static_cast<uint32_t>(y + row) * WIDTH_BYTES + firstByte;
    for (uint16_t byte = 0; byte < widthBytes; ++byte) {
      const uint32_t index = offset + byte;
      const uint8_t black = static_cast<uint8_t>(~fb[index]);
      _glassNonWhite[index] = black;
      _glassBlack[index] = black;
      _lastBw[index] = fb[index];
    }
  }

  // Re-establish equal plane contents for the updated rectangle. This makes
  // the next window activation neutral outside its own changed pixels.
  writePlaneWindow(bus, CMD_WRITE_NEW, fb, x, y, w, h);
  writePlaneWindow(bus, CMD_WRITE_OLD, fb, x, y, w, h);
  restoreFullRamWindow(bus);
  _windowBaselineValid = true;
  _panelHasGray = false;
  _displayCommitted = true;

  if (turnOff) powerOffController(bus);
}

bool PaperMonoDriver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  display(bus, fb, prev, mode, turnOff);
  return false;
}

void PaperMonoDriver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  // displayStart() is synchronous. Balanced grayscale batching uses
  // displayGrayscaleBase()/displayGray() instead of the generic async split.
  (void)bus;
  (void)fb;
}

void PaperMonoDriver::seedPreviousFrame(EpdBus& bus, const uint8_t* buf) {
  if (!buf || !allocateBuffers()) return;
  // There is no host-managed previous-frame plane in this design: the selector
  // planes are rebuilt from _glass* on every activation. Record the caller's
  // baseline so the unchanged-frame test stays honest.
  (void)bus;
  memcpy(_lastBw, buf, BUFFER_SIZE);
  _lastBwValid = true;
}

void PaperMonoDriver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  _preparingGray = true;
  display(bus, fb, nullptr, fallback, turnOff);
  _preparingGray = false;
}

void PaperMonoDriver::beginDisplayWork() {
  _displayWorkGeneration = _abortGeneration.load();
  // Cleared per logical render so displayCommitted() answers "did this page
  // reach the panel", which is what a caller's refresh cadence must key off.
  _displayCommitted = false;
  if (++_renderGeneration == 0) ++_renderGeneration;
  // A logical render owns all three staged planes. Discard anything left by a
  // canceled/OOM render before allowing the next page to contribute strips.
  _pendingTri = false;
  _pendingCorrective = false;
  clearGrayStaging();
}

bool PaperMonoDriver::displayWorkCancelled() const { return _abortGeneration.load() != _displayWorkGeneration; }

bool PaperMonoDriver::postRefreshAborted() const { return displayWorkCancelled(); }

void PaperMonoDriver::abortPostRefresh() { _abortGeneration.fetch_add(1); }

void PaperMonoDriver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (!lsb || !allocateBuffers()) return;
  memcpy(_grayLsb, lsb, BUFFER_SIZE);
  memset(_grayLsbCoverage, 0xFF, sizeof(_grayLsbCoverage));
  _grayLsbRowsCovered = GRAY_ROWS;
  _grayLsbGeneration = _renderGeneration;
  _grayLsbReady = true;
}

void PaperMonoDriver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  if (!msb || !allocateBuffers()) return;
  memcpy(_grayMsb, msb, BUFFER_SIZE);
  memset(_grayMsbCoverage, 0xFF, sizeof(_grayMsbCoverage));
  _grayMsbRowsCovered = GRAY_ROWS;
  _grayMsbGeneration = _renderGeneration;
  _grayMsbReady = true;
}

bool PaperMonoDriver::markGrayRows(GrayPlane plane, uint16_t yStart, uint16_t numRows) {
  uint8_t* coverage = plane == GrayPlane::Lsb ? _grayLsbCoverage : _grayMsbCoverage;
  uint16_t& covered = plane == GrayPlane::Lsb ? _grayLsbRowsCovered : _grayMsbRowsCovered;
  for (uint16_t y = yStart; y < yStart + numRows; ++y) {
    const uint8_t mask = static_cast<uint8_t>(1u << (y & 7));
    uint8_t& slot = coverage[y >> 3];
    if ((slot & mask) == 0) {
      slot = static_cast<uint8_t>(slot | mask);
      ++covered;
    }
  }
  return covered == GRAY_ROWS;
}

void PaperMonoDriver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                             uint16_t numRows) {
  (void)bus;
  if (!rows || numRows == 0 || yStart + numRows > HEIGHT || !allocateBuffers()) return;
  uint8_t* target = plane == GrayPlane::Lsb ? _grayLsb : _grayMsb;
  uint32_t& generation = plane == GrayPlane::Lsb ? _grayLsbGeneration : _grayMsbGeneration;
  bool& ready = plane == GrayPlane::Lsb ? _grayLsbReady : _grayMsbReady;
  if (generation != _renderGeneration) {
    uint8_t* coverage = plane == GrayPlane::Lsb ? _grayLsbCoverage : _grayMsbCoverage;
    uint16_t& covered = plane == GrayPlane::Lsb ? _grayLsbRowsCovered : _grayMsbRowsCovered;
    memset(coverage, 0, GRAY_COVERAGE_BYTES);
    covered = 0;
    ready = false;
    generation = _renderGeneration;
  }
  memcpy(target + static_cast<uint32_t>(yStart) * WIDTH_BYTES, rows, static_cast<uint32_t>(numRows) * WIDTH_BYTES);
  ready = markGrayRows(plane, yStart, numRows);
}

void PaperMonoDriver::prepareGrayscaleTarget(const uint8_t* bw) {
  // Both selector planes and the two-level target are staged in host RAM.
  // displayGray() validates complete, current-generation planes before use.
  (void)bw;
}

void PaperMonoDriver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)turnOff;
  (void)lut;
  (void)factoryMode;
  if (!allocateBuffers()) return;

  if (displayWorkCancelled()) {
    // Input arrived while this page was being composed. Drop it whole: the
    // panel still shows the previous page, which is a valid state, and the
    // replacement page is already on its way. Nothing was submitted, so there
    // is no partial frame to repair.
    _pendingTri = false;
    _pendingCorrective = false;
    clearGrayStaging();
    return;
  }

  const bool useGray = _grayLsbReady && _grayMsbReady &&
                       _grayLsbGeneration == _renderGeneration &&
                       _grayMsbGeneration == _renderGeneration;
  if (_pendingTri) {
    commitPending(bus, useGray);
    return;
  }
  // No two-level target is outstanding (a caller displayed one separately).
  // `fb` is not a reliable B/W target here: the full-frame text AA path reuses
  // the live framebuffer for its gray selector planes before calling us. Apply
  // those selectors to the preserved B/W page instead and do not poison the
  // baseline with selector bytes.
  if (!useGray || !_lastBwValid) {
    clearGrayStaging();
    return;
  }
  // The base page is already on the glass (the host displayed it without
  // displayGrayscaleBase(), so commitPending consumed the stash above). Develop
  // the AA grays as a changed-pixels-only overlay: driving the full non-white
  // body a second time through the kick phases reads as a page-wide flash.
  runUpdate(bus, _lastBw, true, false, /*overlayOnly=*/true);
  clearGrayStaging();
}

void PaperMonoDriver::displayGrayCalibration(EpdBus& bus, const uint8_t* fb, uint16_t customX, uint16_t customY,
                                           uint16_t customW, uint16_t customH) {
  // There is a single gray tone and a single waveform now, so a side-by-side
  // "reference vs current" split has nothing left to compare. Render the whole
  // frame through the production path instead, driving every pixel so the
  // result does not depend on what was on the panel before.
  (void)customX;
  (void)customY;
  (void)customW;
  (void)customH;
  if (!fb || !allocateBuffers()) return;
  memcpy(_pendingBw, fb, BUFFER_SIZE);
  _pendingTri = true;
  _pendingCorrective = true;
  _pendingGeneration = _renderGeneration;
  const bool useGray = _grayLsbReady && _grayMsbReady &&
                       _grayLsbGeneration == _renderGeneration &&
                       _grayMsbGeneration == _renderGeneration;
  commitPending(bus, useGray);
}

void PaperMonoDriver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!_pendingTri) {
    clearGrayStaging();
    return;
  }
  if (displayWorkCancelled()) {
    _pendingTri = false;
    _pendingCorrective = false;
    clearGrayStaging();
    return;
  }
  // displayGray() bailed without consuming the stashed target (no selectors
  // were ever staged, for instance). The frame must still reach the panel.
  if (bw && _pendingBw) memcpy(_pendingBw, bw, BUFFER_SIZE);
  const bool useGray = _grayLsbReady && _grayMsbReady &&
                       _grayLsbGeneration == _renderGeneration &&
                       _grayMsbGeneration == _renderGeneration;
  commitPending(bus, useGray);
}

void PaperMonoDriver::controllerIdle(EpdBus& bus) {
  if (!_initialized) return;
  powerOffController(bus);

  // The controller loses its RAM and registers here, and that costs nothing:
  // both selector planes are rebuilt from the host-side glass model on every
  // activation, so waking is one reset pulse plus initController(), about 40 ms,
  // and only ever after the user has already stopped turning pages.
  bus.cmd(0x10);
  bus.data(0x03);  // SSD1677 deep-sleep key; 0x01 does not enter sleep.
  delay(2);
  _initialized = false;
  _controllerPowered = false;
  _windowBaselineValid = false;
}

void PaperMonoDriver::setGrayParams(const PaperMonoGrayParams& params) {
  _grayParams = params;
  if (_grayParams.lightFrames == 0) _grayParams.lightFrames = 1;
  if (_grayParams.lightFrames > 12) _grayParams.lightFrames = 12;
  // Stored presets may still carry a nonzero polish count; the polish is
  // retired (see PaperMonoGrayParams), so it is not forwarded to the waveform.
  _tri.postCleanCycles = 0;

  // lightFrames selects the middle tone: tGray weak-rail frames applied to a
  // saturated-white pixel. The full weak-rail swing is ~60 frames, so the
  // default 11 -> 24 frames lands near 40% -- a tone the eye separates from
  // both endpoints even on thin CJK strokes. Quantised to a multiple of three
  // so the gray class's white reset (kick + tGray/3) stays integral and the
  // class stays exactly DC balanced.
  const uint16_t requested = static_cast<uint16_t>(2 * _grayParams.lightFrames + 2);
  const uint16_t clamped = std::clamp<uint16_t>(requested, SAT_MIN_FRAMES / 2, 60);
  _tri.tGray = static_cast<uint8_t>(((clamped + 1) / 3) * 3);
}

void PaperMonoDriver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;
  _needsFull = true;
  _lastBwValid = false;
  resetGray();
}

void PaperMonoDriver::resetGray() {
  _panelHasGray = false;
  clearGrayStaging();
  _pendingTri = false;
  _pendingCorrective = false;
  _pendingGeneration = 0;
}

void PaperMonoDriver::clearGrayStaging() {
  _grayLsbReady = false;
  _grayMsbReady = false;
  _grayLsbGeneration = 0;
  _grayMsbGeneration = 0;
  _grayLsbRowsCovered = 0;
  _grayMsbRowsCovered = 0;
  memset(_grayLsbCoverage, 0, sizeof(_grayLsbCoverage));
  memset(_grayMsbCoverage, 0, sizeof(_grayMsbCoverage));
}

void PaperMonoDriver::deepSleep(EpdBus& bus) {
  // controllerIdle() may already have parked the controller in the same deep
  // sleep mode. The register writes are then pointless, but the host-side state
  // reset below is not: this is the path after which EPD power is cut, so the
  // on-glass image can no longer be trusted.
  if (_initialized) {
    bus.waitBusy("PaperMono idle");
    powerOffController(bus);
    bus.cmd(0x10);
    bus.data(0x03);
    delay(100);
  }
  _initialized = false;
  _needsFull = true;
  _lastBwValid = false;
  _windowBaselineValid = false;
  resetGray();
}

}  // namespace freeink
