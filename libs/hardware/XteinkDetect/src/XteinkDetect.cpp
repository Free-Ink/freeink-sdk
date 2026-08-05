#include "XteinkDetect.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Wire.h>

#include <string.h>

#include "nvs.h"

// Two independent capabilities live in this file:
//   * FREEINK_XTEINK_C3 — the X3-vs-X4 I2C fingerprint on SDA=20 / SCL=0. Only
//     safe on the Xteink C3 pinout (on an S3 those pins are USB D+ / boot
//     strap), so it compiles only for the C3 profiles.
//   * FREEINK_XTEINK_DISPLAY_PROBE — the board-agnostic UC81xx display-controller
//     fingerprint. It reads whatever pins the ACTIVE profile carries, so it is
//     safe on the S3 X4 Pro too.
#define FREEINK_XTEINK_C3 (FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3)
#define FREEINK_XTEINK_DISPLAY_PROBE (FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4 || FREEINK_DEVICE_X4PRO)

namespace freeink {

// =============================================================================
// Board-agnostic UC81xx display-controller probe (shared bit-bang core).
// =============================================================================
#if FREEINK_XTEINK_DISPLAY_PROBE

namespace {

struct EpdProbePins {
  int8_t sclk;
  int8_t mosi;  // the controller's bidirectional SDA in half-duplex mode
  int8_t cs;
  int8_t dc;
  int8_t rst;
  int8_t busy;
};

// UC81xx read-capable registers (UC8179 / UC8279d datasheets, identical layout).
constexpr uint8_t UC81XX_CMD_VER = 0x70;   // reserved 0x00, CHIP_VER, LUT_VER[23:0]
constexpr uint8_t UC81XX_CMD_FLG = 0x71;   // status; BUSY_N (D0) = 1 when idle
constexpr uint8_t UC81XX_CMD_RMTP = 0xA2;  // bulk MTP read: 1 dummy byte, then MTP[0..n]

// Diagnostics snapshot of the last probe (see XteinkDisplayProbeDiag in the
// header). File-scope so locked-unit firmware can persist it after the fact.
XteinkDisplayProbeDiag g_probeDiag;

inline void epdClockDelay() { delayMicroseconds(1); }  // ~500 kHz, timing-safe

void epdWriteByte(const EpdProbePins& p, uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(p.mosi, (b & 0x80) ? HIGH : LOW);
    epdClockDelay();
    digitalWrite(p.sclk, HIGH);
    epdClockDelay();
    digitalWrite(p.sclk, LOW);
    b <<= 1;
  }
}

uint8_t epdReadByte(const EpdProbePins& p) {
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) {
    // The controller shifts the next bit out on the SCL falling edge; sample
    // while the clock is low, then pulse.
    epdClockDelay();
    b = static_cast<uint8_t>((b << 1) | (digitalRead(p.mosi) == HIGH ? 1 : 0));
    digitalWrite(p.sclk, HIGH);
    epdClockDelay();
    digitalWrite(p.sclk, LOW);
  }
  return b;
}

// One command + N-byte half-duplex read: command with DC low, then SDA (our
// MOSI) released to input with DC high while the controller drives the reads.
void epdCmdRead(const EpdProbePins& p, uint8_t cmd, uint8_t* out, uint8_t len) {
  pinMode(p.mosi, OUTPUT);
  digitalWrite(p.dc, LOW);
  digitalWrite(p.cs, LOW);
  epdClockDelay();
  epdWriteByte(p, cmd);
  digitalWrite(p.dc, HIGH);
  pinMode(p.mosi, INPUT_PULLUP);
  epdClockDelay();
  for (uint8_t i = 0; i < len; i++) out[i] = epdReadByte(p);
  digitalWrite(p.cs, HIGH);
  pinMode(p.mosi, OUTPUT);
}

// The UC81xx VER signature: a leading reserved 0x00, then a CHIP_VER byte
// (datasheet default 0x03, MTP-programmed so not pinned to that exact value)
// that is neither a floating-low nor a floating-high bus. A released SDA reads
// back all-0x00 or all-0xFF through the pull-up, and neither the UC8253 nor the
// SSD1677 answers 0x70 with this shape (the SSD-family has no such read), so a
// additionally report idle (BUSY_N=1) without being a floating pattern.
//
// Match on the FLG status plus a non-uniform VER. A UC81xx drives 0x71 to a real
// status byte (BUSY_N=D0=1 when idle) and returns a structured VER; an SSD-family
// controller doesn't answer 0x70/0x71 at all, so the half-duplex line floats to a
// uniform level (all 0xFF via the pull-up, or all 0x00). We deliberately do NOT
// require any specific CHIP_VER value: a shipping X4 Pro UC8179 was observed
// returning VER=00 00 01 FF FF (CHIP_VER byte = 0x00) with FLG=0x13 — an earlier
// matcher that required ver[1] != 0 wrongly rejected it.
bool verIsFloating(const uint8_t ver[5]) {
  for (int i = 1; i < 5; i++)
    if (ver[i] != ver[0]) return false;  // any variation => a real, driven response
  return true;                           // all five bytes identical => floating bus
}

bool matchUc81xx(const uint8_t ver[5], uint8_t flg) {
  // FLG must be a real, non-floating status with BUSY_N (bit0) asserted (idle).
  if (flg == 0x00 || flg == 0xFF) return false;
  if ((flg & 0x01) != 0x01) return false;
  // VER must be an actually-driven (non-uniform) pattern, not a floating bus.
  return !verIsFloating(ver);
}

bool runDisplayProbePass(const EpdProbePins& p, uint8_t ver[5], uint8_t* flg, uint8_t rstLowMs) {
  pinMode(p.cs, OUTPUT);
  digitalWrite(p.cs, HIGH);
  pinMode(p.sclk, OUTPUT);
  digitalWrite(p.sclk, LOW);
  pinMode(p.dc, OUTPUT);
  digitalWrite(p.dc, LOW);
  pinMode(p.mosi, OUTPUT);
  if (p.busy >= 0) pinMode(p.busy, INPUT);

  // Hardware reset pulse (rstLowMs low, then a fixed settle). The vendor
  // identification path holds RST_N low for 50 ms (well beyond the datasheet's
  // 50 us minimum — the ID readback is less forgiving than normal operation),
  // but that cost is only paid on the CONFIRM pass: the screening pass uses a
  // short pulse so the common case — an SSD-family panel that will never answer
  // 0x70 — doesn't add ~100 ms to every boot and wake (see
  // probeDisplayController). We can't trust BUSY polarity here — the controller
  // (and therefore its idle level) is exactly what we're trying to identify —
  // so we don't gate on BUSY; a flat delay covers every UC81xx power-up. The
  // panel driver's own begin() resets again afterwards, so this leaves no state.
  if (p.rst >= 0) {
    pinMode(p.rst, OUTPUT);
    digitalWrite(p.rst, HIGH);
    delay(2);
    digitalWrite(p.rst, LOW);
    delay(rstLowMs);
    digitalWrite(p.rst, HIGH);
  }
  delay(30);

  uint8_t flgByte = 0;
  epdCmdRead(p, UC81XX_CMD_FLG, &flgByte, 1);
  epdCmdRead(p, UC81XX_CMD_VER, ver, 5);
  if (flg) *flg = flgByte;
  return matchUc81xx(ver, flgByte);
}

void releaseDisplayPins(const EpdProbePins& p) {
  // Leave everything released. RST_N has an internal pull-up, so INPUT keeps the
  // controller out of reset.
  pinMode(p.sclk, INPUT);
  pinMode(p.mosi, INPUT);
  pinMode(p.cs, INPUT_PULLUP);  // don't leave the panel selected
  pinMode(p.dc, INPUT);
  if (p.rst >= 0) pinMode(p.rst, INPUT);
}

// Two-pass probe with agreement, over an arbitrary pinout. Confirmed only when
// both passes match the UC81xx signature AND agree on the VER bytes — a floating
// bus can't produce the same stable non-trivial pattern twice. Disagreement is
// Inconclusive; both-fail is PrimaryAssumed (the profile's default controller).
//
// Reset budget: pass 1 screens with the short (1 ms) reset that every benched
// UC81xx answers fine; only if it matches does pass 2 confirm with the vendor
// identification timing (RST low 50 ms), which also makes pass 2's VER the one
// read under doc conditions. An SSD-family board (floating bus) therefore pays
// two cheap passes (~66 ms total, as before the doc-timing change) instead of
// two 50 ms resets on every boot and wake.
DisplayControllerVerdict probeDisplayController(const EpdProbePins& p, uint8_t verBytes[5], uint8_t* flg,
                                                bool escalateReset) {
  uint8_t ver1[5] = {0};
  uint8_t ver2[5] = {0};
  uint8_t flg1 = 0;
  bool pass1 = runDisplayProbePass(p, ver1, &flg1, /*rstLowMs=*/1);
  if (!pass1 && escalateReset) {
    // Escalation for boards whose UC sibling might only answer the vendor's
    // identification timing (RST low 50 ms): a failed short screening pass is
    // retried once at doc timing before concluding "no UC part". Requested for
    // X3-family boards (their boot budget tolerates it); the X4 family keeps
    // the cheap path — its UC8179 is bench-proven to answer the 1 ms pulse.
    delay(2);
    pass1 = runDisplayProbePass(p, ver1, &flg1, /*rstLowMs=*/50);
  }
  delay(2);
  const bool pass2 = runDisplayProbePass(p, ver2, nullptr, /*rstLowMs=*/pass1 ? 50 : 1);

  const bool verAgree = memcmp(ver1, ver2, 5) == 0;
  bool confirmed = pass1 && pass2 && verAgree;
  // FLG is a driven idle status (not a floating 0xFF / dead 0x00, BUSY_N set).
  const bool flgDriven = flg1 != 0x00 && flg1 != 0xFF && (flg1 & 0x01) == 0x01;

  // Diagnostics snapshot for locked units (persisted by firmware, e.g. to SD).
  g_probeDiag.valid = true;
  memcpy(g_probeDiag.ver, pass1 && pass2 ? ver2 : ver1, 5);
  g_probeDiag.flg = flg1;
  g_probeDiag.promoted = false;
  g_probeDiag.mtpValid = false;

  // Ground-truth dump of the module's factory configuration: RMTP (0xA2)
  // returns a dummy byte then MTP[0..n] — the 0xA5 refresh-enable key, the
  // Command Default Setting block (real PSR/TRES/GSST/CDI/TCON), product ID
  // and LUT version. Read whenever SOMETHING is driving the status line: on a
  // confirmed part it's diagnostics; on the fallback path below it is the
  // discriminator itself. A part without RMTP (UC8253, SSD-family) floats the
  // line and reads uniform garbage here.
  if (confirmed || flgDriven) {
    uint8_t raw[sizeof(g_probeDiag.mtp) + 1] = {0};
    epdCmdRead(p, UC81XX_CMD_RMTP, raw, sizeof(raw));
    memcpy(g_probeDiag.mtp, raw + 1, sizeof(g_probeDiag.mtp));
    g_probeDiag.mtpValid = true;
  }

  // Fallback match — FIELD-OBSERVED UC8279d signature: new X3 units return
  // VER = FF FF FF FF FF (blank/unreadable LUT_VER area) with FLG = 0x13 (the
  // datasheet's idle default), which the uniform-VER floating-bus test wrongly
  // rejects. A pulled-up floating bus also reads FF — so require POSITIVE
  // evidence: the RMTP dump must start with the 0xA5 MTP key, which only a
  // real UC81xx with a programmed MTP can produce (and without which OTP-mode
  // refreshes wouldn't run anyway). UC8253 has no RMTP command; its read
  // floats and can never match.
  if (!confirmed && flgDriven && verAgree && verIsFloating(ver1) && ver1[0] == 0xFF && g_probeDiag.mtpValid &&
      g_probeDiag.mtp[0] == 0xA5) {
    confirmed = true;
  }
  releaseDisplayPins(p);

  if (verBytes) memcpy(verBytes, g_probeDiag.ver, 5);
  if (flg) *flg = flg1;
  DisplayControllerVerdict v = DisplayControllerVerdict::Inconclusive;
  if (confirmed) v = DisplayControllerVerdict::Uc81xxConfirmed;
  else if (!pass1 && !pass2) v = DisplayControllerVerdict::PrimaryAssumed;
  g_probeDiag.verdict = static_cast<uint8_t>(v);
  return v;
}

}  // namespace

DisplayControllerVerdict detectXteinkDisplayController(uint8_t verBytes[5], uint8_t* flg) {
  const auto& d = BoardConfig::ACTIVE.display;
  const EpdProbePins p{d.sclk, d.mosi, d.cs, d.dc, d.rst, d.busy};
  // X3-family boards (UC8253 default) escalate a failed screening pass to the
  // 50 ms vendor-ID reset — see probeDisplayController.
  const bool escalate = BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8253;
  return probeDisplayController(p, verBytes, flg, escalate);
}

const XteinkDisplayProbeDiag& getXteinkDisplayProbeDiag() { return g_probeDiag; }

namespace {

// The OEM records the panel controller per unit in NVS namespace `hw_calib`,
// key `screenType` (u8): 1/0x0B = UC8179, 2/0x0C = UC8279, anything else (incl.
// the default 3) = the shipping SSD-family / UC8253 part. We READ it only for
// diagnostics/cross-reference — NOT for the decision. It is unreliable in the
// field: a full-flash from another unit overwrites this namespace, so it can
// describe the wrong panel entirely. The live bus probe is the ground truth.
// Returns false if NVS has no such namespace/key.
bool readOemScreenType(uint8_t* out) {
  nvs_handle_t h;
  if (nvs_open("hw_calib", NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t v = 0;
  const esp_err_t e = nvs_get_u8(h, "screenType", &v);
  nvs_close(h);
  if (e != ESP_OK) return false;
  *out = v;
  return true;
}

bool screenTypeIsUltraChip(uint8_t st) { return st == 1 || st == 2 || st == 0x0B || st == 0x0C; }

// Run the display-bus probe and report the verdict with a diagnostic log line.
// On a confirmed UltraChip part, `verOut` receives the 5 VER bytes (byte2 is
// LUT_VER, which identifies the silicon variant).
bool probeSaysUltraChip(uint8_t verOut[5]) {
  uint8_t ver[5] = {0};
  uint8_t flg = 0;
  const DisplayControllerVerdict v = detectXteinkDisplayController(ver, &flg);
  memcpy(verOut, ver, 5);
  if (Serial) {
    Serial.printf("[%lu] [XTDET] bus probe VER=%02X %02X %02X %02X %02X FLG=%02X -> %s\n", millis(), ver[0], ver[1],
                  ver[2], ver[3], ver[4], flg,
                  v == DisplayControllerVerdict::Uc81xxConfirmed  ? "UltraChip"
                  : v == DisplayControllerVerdict::PrimaryAssumed ? "default controller"
                                                                  : "inconclusive (default)");
    // MTP header (RMTP 0xA2), read whenever the status line was driven: 0xA5 at
    // byte 0 = a UC part with a programmed MTP (the fallback discriminator);
    // uniform FF/00 = no RMTP support (UC8253 / SSD-family) or unreadable.
    if (g_probeDiag.mtpValid) {
      Serial.printf("[%lu] [XTDET] MTP[0x000..0x02F]:", millis());
      for (size_t i = 0; i < sizeof(g_probeDiag.mtp); i++) Serial.printf(" %02X", g_probeDiag.mtp[i]);
      Serial.printf("\n");
    }
  }
  return v == DisplayControllerVerdict::Uc81xxConfirmed;
}

}  // namespace

bool applyXteinkDisplayController() {
  // Decide from the live display-bus probe — the ground truth. The OEM NVS
  // hw_calib/screenType is read only for diagnostics: it's unreliable in the
  // field (a full-flash from another unit overwrites it, so it can name the wrong
  // panel). Log it — and flag when it disagrees with the probe — but never
  // decide on it.
  uint8_t screenType = 0;
  if (readOemScreenType(&screenType)) {
    if (Serial)
      Serial.printf("[%lu] [XTDET] NVS hw_calib/screenType=%u (%s) [info only]\n", millis(), screenType,
                    screenTypeIsUltraChip(screenType) ? "UltraChip" : "default");
  } else if (Serial) {
    Serial.printf("[%lu] [XTDET] NVS hw_calib/screenType: not set [info only]\n", millis());
  }

  uint8_t ver[5] = {0};
  const bool ultraChip = probeSaysUltraChip(ver);
  if (!ultraChip) return false;

  // Promote the profile's default controller to its UltraChip sibling.
  switch (BoardConfig::ACTIVE.displayController) {
    case BoardConfig::DisplayController::SSD1677: {
      // X4-family boards can carry either UltraChip part; VER byte2 (LUT_VER)
      // tells them apart per the vendor reference: 0x01 = UC8179, 0x02/0x68 =
      // UC8279 (800x480 variant), 0x69 = reserved UC8279. Anything else is
      // unrecognized — take the UC8179 driver, the variant every unit benched
      // so far has carried (observed VER=00 00 01 FF FF).
      const uint8_t lutVer = ver[2];
      g_probeDiag.promoted = true;
      if (lutVer == 0x02 || lutVer == 0x68 || lutVer == 0x69) {
        BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8279;
        BoardConfig::ACTIVE.displayControllerVariant = lutVer;
        if (Serial)
          Serial.printf("[%lu] [XTDET] promoted SSD1677 -> UC8279 800x480 (LUT_VER=%02X%s)\n", millis(), lutVer,
                        lutVer == 0x69 ? ", reserved" : "");
      } else {
        BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8179;
        BoardConfig::ACTIVE.displayControllerVariant = lutVer;
        if (Serial)
          Serial.printf("[%lu] [XTDET] promoted SSD1677 -> UC8179 (LUT_VER=%02X%s)\n", millis(), lutVer,
                        lutVer == 0x01 ? "" : ", unrecognized -> UC8179 default");
      }
      return true;
    }
    case BoardConfig::DisplayController::UC8253:
      BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8279;
      g_probeDiag.promoted = true;
      if (Serial) Serial.printf("[%lu] [XTDET] promoted UC8253 -> UC8279\n", millis());
      return true;
    default:
      return false;  // already an UltraChip part (or a non-sibling default)
  }
}

#else  // no probe-capable profile in this build

DisplayControllerVerdict detectXteinkDisplayController(uint8_t verBytes[5], uint8_t* flg) {
  if (verBytes) memset(verBytes, 0, 5);
  if (flg) *flg = 0;
  return DisplayControllerVerdict::PrimaryAssumed;
}
bool applyXteinkDisplayController() { return false; }

const XteinkDisplayProbeDiag& getXteinkDisplayProbeDiag() {
  static const XteinkDisplayProbeDiag empty;
  return empty;
}

#endif  // FREEINK_XTEINK_DISPLAY_PROBE

// =============================================================================
// X3-vs-X4 I2C fingerprint (C3-only) + X3 display-controller wrapper.
// =============================================================================
#if !FREEINK_XTEINK_C3

// No C3 Xteink profile in this build, so there is nothing to fingerprint over
// the C3-only I2C bus, and no X3 disambiguation to do.
XteinkVerdict detectXteinkVerdict(uint8_t* score1, uint8_t* score2) {
  if (score1) *score1 = 0;
  if (score2) *score2 = 0;
  return XteinkVerdict::Inconclusive;
}
bool detectXteinkIsX3() { return false; }
X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  if (verBytes) memset(verBytes, 0, 5);
  if (flg) *flg = 0;
  return X3DisplayVerdict::Uc8253Assumed;
}
bool selectXteinkDevice() { return false; }

#else

namespace {

// X3-only peripherals on the secondary I2C bus (SDA=20, SCL=0).
constexpr int X3_I2C_SDA = 20;
constexpr int X3_I2C_SCL = 0;
constexpr uint32_t X3_I2C_FREQ = 400000;

constexpr uint8_t ADDR_BQ27220 = 0x55;  // fuel gauge
constexpr uint8_t ADDR_DS3231 = 0x68;   // RTC
constexpr uint8_t ADDR_QMI8658 = 0x6B;  // IMU
constexpr uint8_t ADDR_QMI8658_ALT = 0x6A;

constexpr uint8_t BQ27220_SOC_REG = 0x2C;
constexpr uint8_t BQ27220_VOLT_REG = 0x08;
constexpr uint8_t DS3231_SEC_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;

bool readReg8(uint8_t addr, uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  *out = Wire.read();
  return true;
}

bool readReg16LE(uint8_t addr, uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *out = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

// Each probe checks not just for an ACK but for a plausible value, so a stray
// pull-up or floating bus can't masquerade as a present chip.
bool probeBq27220() {
  uint16_t soc = 0;
  uint16_t mv = 0;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_SOC_REG, &soc) || soc > 100) return false;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_VOLT_REG, &mv)) return false;
  return mv >= 2500 && mv <= 5000;
}

bool probeDs3231() {
  uint8_t sec = 0;
  if (!readReg8(ADDR_DS3231, DS3231_SEC_REG, &sec)) return false;
  const uint8_t tens = (sec >> 4) & 0x07;
  const uint8_t ones = sec & 0x0F;
  return tens <= 5 && ones <= 9;  // valid BCD seconds
}

bool probeQmi8658() {
  uint8_t who = 0;
  if (readReg8(ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  if (readReg8(ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  return false;
}

uint8_t runProbePass() {
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);
  const uint8_t score =
      static_cast<uint8_t>(probeBq27220()) + static_cast<uint8_t>(probeDs3231()) + static_cast<uint8_t>(probeQmi8658());
  Wire.end();
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
  return score;
}

}  // namespace

XteinkVerdict detectXteinkVerdict(uint8_t* score1, uint8_t* score2) {
  const uint8_t pass1 = runProbePass();
  delay(2);
  const uint8_t pass2 = runProbePass();
  if (score1) *score1 = pass1;
  if (score2) *score2 = pass2;
  // X3 confirmed only when both passes see at least two of the three chips; the
  // X4 sees zero, so a single stray ACK never flips the result. Anything in
  // between is Inconclusive: callers should run as X4 but may re-probe later.
  if (pass1 >= 2 && pass2 >= 2) return XteinkVerdict::X3Confirmed;
  if (pass1 == 0 && pass2 == 0) return XteinkVerdict::X4Confirmed;
  return XteinkVerdict::Inconclusive;
}

bool detectXteinkIsX3() { return detectXteinkVerdict() == XteinkVerdict::X3Confirmed; }

#if FREEINK_DEVICE_X3

X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  // The X3 uses the shared board-agnostic probe over its display pinout. Both
  // X3 profiles carry the same display pins, so ACTIVE (still the boot default
  // here, before selectDevice) has the right map either way.
  const DisplayControllerVerdict v = detectXteinkDisplayController(verBytes, flg);
  switch (v) {
    case DisplayControllerVerdict::Uc81xxConfirmed: return X3DisplayVerdict::Uc8279Confirmed;
    case DisplayControllerVerdict::PrimaryAssumed: return X3DisplayVerdict::Uc8253Assumed;
    default: return X3DisplayVerdict::Inconclusive;
  }
}

#else  // X4-only C3 build: no X3 profile, nothing to disambiguate.

X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  if (verBytes) memset(verBytes, 0, 5);
  if (flg) *flg = 0;
  return X3DisplayVerdict::Uc8253Assumed;
}

#endif  // FREEINK_DEVICE_X3

bool selectXteinkDevice() {
  const bool isX3 = detectXteinkIsX3();
  if (!isX3) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
    return false;
  }
  // X3 confirmed: fingerprint which panel controller this production run
  // carries and select the matching sibling profile.
  const bool isUc8279 = detectX3DisplayController() == X3DisplayVerdict::Uc8279Confirmed;
  BoardConfig::selectDevice(isUc8279 ? BoardConfig::Board::XteinkX3Uc8279 : BoardConfig::Board::XteinkX3);
  return true;
}

#endif  // FREEINK_XTEINK_C3

}  // namespace freeink
