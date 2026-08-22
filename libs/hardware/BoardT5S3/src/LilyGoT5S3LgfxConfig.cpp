#include <BoardT5S3.h>
#include <ED047TC2Waveform.h>
#include <LgfxEpdConfig.h>
#include <Wire.h>

namespace {

constexpr int kDefaultVcomMv = -1600;
constexpr uint8_t kTpsRegTmstValue = 0x00;
constexpr uint8_t kTpsRegEnable = 0x01;
constexpr uint8_t kTpsRegVcom = 0x03;
constexpr uint8_t kTpsRegTmst1 = 0x0D;
constexpr uint8_t kTpsRegPowerGood = 0x0F;
constexpr uint8_t kTpsEnableOutputs = 0x3F;
constexpr uint8_t kTpsStartConversion = 0x80;  // TMST1 READ_THERM
constexpr uint8_t kTpsConversionDone = 0x20;   // TMST1 CONV_END

// Used when the PMIC thermistor cannot be read. Room temperature sits in the
// middle of the waveform's range, so a wrong guess is off by at most a few
// frames either way.
constexpr int kAssumedTemperatureC = 25;

bool writeTpsRegister(uint8_t reg, const uint8_t* data, size_t len) {
  BoardT5S3::ScopedI2CLock lock;
  Wire.beginTransmission(T5S3_TPS65185_ADDR);
  Wire.write(reg);
  if (data && len) Wire.write(data, len);
  return Wire.endTransmission() == 0;
}

bool writeTpsRegister8(uint8_t reg, uint8_t value) { return writeTpsRegister(reg, &value, 1); }

bool readTpsRegister(uint8_t reg, uint8_t* data, size_t len) {
  if (!data || !len) return false;
  BoardT5S3::ScopedI2CLock lock;
  Wire.beginTransmission(T5S3_TPS65185_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  const uint8_t want = static_cast<uint8_t>(len);
  if (Wire.requestFrom(static_cast<uint8_t>(T5S3_TPS65185_ADDR), want) != want) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < len; ++i) data[i] = Wire.read();
  return true;
}

// Ambient temperature in degrees C from the PMIC's thermistor.
//
// Wait for READ_THERM to self-clear, not for CONV_END. CONV_END latches and is
// still set from whatever conversion ran before, so a loop that breaks on it
// alone returns on the very first poll and reads TMST_VALUE before this
// conversion has produced anything -- which is how this came back a flat 0 C and
// pinned the waveform to its coldest, longest range no matter how warm the panel
// actually was.
bool readTpsThermistor(int8_t* out) {
  if (!writeTpsRegister8(kTpsRegTmst1, kTpsStartConversion)) return false;
  for (int tries = 0; tries < 100; ++tries) {
    delay(2);
    uint8_t status = 0;
    if (!readTpsRegister(kTpsRegTmst1, &status, 1)) return false;
    if ((status & kTpsStartConversion) == 0 && (status & kTpsConversionDone) != 0) {
      uint8_t value = 0;
      if (!readTpsRegister(kTpsRegTmstValue, &value, 1)) return false;
      *out = static_cast<int8_t>(value);
      return true;
    }
  }
  return false;
}

// The PMIC answers I2C as soon as WAKEUP is high, well before the high-voltage
// rails come up, so the panel temperature can be sampled without driving the
// panel at all. That matters because this runs before the EPD bus exists: the
// waveform has to be chosen before LovyanGFX expands it at panel init.
bool readPanelTemperature(int8_t* out) {
  if (!BoardT5S3::setPca9535PinMode(PCA9535_IO15_TPS_WAKEUP, OUTPUT)) return false;
  if (!BoardT5S3::writePca9535Pin(PCA9535_IO15_TPS_WAKEUP, true)) return false;
  delay(10);  // PMIC wakeup, then its thermistor block settles
  const bool ok = readTpsThermistor(out);
  BoardT5S3::writePca9535Pin(PCA9535_IO15_TPS_WAKEUP, false);
  return ok;
}

bool waitForPcaPinHigh(uint8_t pin, uint32_t timeoutMs) {
  const uint32_t start = millis();
  bool high = false;
  while (millis() - start < timeoutMs) {
    if (BoardT5S3::readPca9535Pin(pin, &high) && high) return true;
    delay(1);
  }
  return false;
}

bool waitForTpsReady(uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint8_t powerGood = 0;
  while (millis() - start < timeoutMs) {
    if (readTpsRegister(kTpsRegPowerGood, &powerGood, 1) && (powerGood & 0xFA) == 0xFA) return true;
    delay(1);
  }
  return false;
}

bool prepareEpdPower() {
  pinMode(EP_STV, OUTPUT);
  digitalWrite(EP_STV, LOW);

  bool ok = true;
  ok &= BoardT5S3::setPca9535PinMode(PCA9535_IO10_EP_OE, OUTPUT);
  ok &= BoardT5S3::setPca9535PinMode(PCA9535_IO11_EP_MODE, OUTPUT);
  ok &= BoardT5S3::setPca9535PinMode(PCA9535_IO13_TPS_PWRUP, OUTPUT);
  ok &= BoardT5S3::setPca9535PinMode(PCA9535_IO14_VCOM_CTRL, OUTPUT);
  ok &= BoardT5S3::setPca9535PinMode(PCA9535_IO15_TPS_WAKEUP, OUTPUT);
  ok &= BoardT5S3::setPca9535PinMode(PCA9535_IO16_TPS_PWR_GOOD, INPUT);
  ok &= BoardT5S3::setPca9535PinMode(PCA9535_IO17_TPS_INT, INPUT);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO10_EP_OE, false);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO11_EP_MODE, false);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO13_TPS_PWRUP, false);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO14_VCOM_CTRL, false);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO15_TPS_WAKEUP, false);
  return ok;
}

void epdPowerOff() {
  BoardT5S3::writePca9535Pin(PCA9535_IO10_EP_OE, false);
  BoardT5S3::writePca9535Pin(PCA9535_IO11_EP_MODE, false);
  BoardT5S3::writePca9535Pin(PCA9535_IO13_TPS_PWRUP, false);
  BoardT5S3::writePca9535Pin(PCA9535_IO14_VCOM_CTRL, false);
  delay(1);
  BoardT5S3::writePca9535Pin(PCA9535_IO15_TPS_WAKEUP, false);
  digitalWrite(EP_STV, LOW);
}

bool epdPowerOn() {
  digitalWrite(EP_STV, HIGH);

  bool ok = true;
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO10_EP_OE, true);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO11_EP_MODE, true);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO15_TPS_WAKEUP, true);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO13_TPS_PWRUP, true);
  ok &= BoardT5S3::writePca9535Pin(PCA9535_IO14_VCOM_CTRL, true);
  if (!ok) {
    epdPowerOff();
    return false;
  }

  delay(1);
  if (!waitForPcaPinHigh(PCA9535_IO16_TPS_PWR_GOOD, 400)) {
    epdPowerOff();
    return false;
  }
  if (!writeTpsRegister8(kTpsRegEnable, kTpsEnableOutputs)) {
    epdPowerOff();
    return false;
  }

  const uint16_t vcom = static_cast<uint16_t>(-kDefaultVcomMv / 10);
  const uint8_t vcomBytes[2] = {static_cast<uint8_t>(vcom & 0xFF), static_cast<uint8_t>(vcom >> 8)};
  if (!writeTpsRegister(kTpsRegVcom, vcomBytes, sizeof(vcomBytes))) {
    epdPowerOff();
    return false;
  }
  if (!waitForTpsReady(400)) {
    epdPowerOff();
    return false;
  }
  return true;
}

freeink::LgfxEpdConfig buildConfig() {
  int8_t measured = 0;
  const bool measuredOk = readPanelTemperature(&measured);
  const int tempC = measuredOk ? measured : kAssumedTemperatureC;
  // Slots for modes this stack never refreshes with. CrossPoint maps FAST to
  // epd_fast and HALF/FULL to epd_text; epd_quality is only ever a WRITE mode
  // (graded pixel quantizer -- the write path reads no LUT) and epd_fastest is
  // unused. Panel_EPD would fill an empty slot with LovyanGFX's stock tables,
  // which both burns LUT blocks and is tuned for another panel; a one-row
  // terminator keeps the slot valid and nearly free.
  //
  // The blocks matter more than they look: Panel_EPD stores per-pixel progress
  // in uint16_t words and flags the fast modes with +0x8000, so the fast and
  // fastest banks must START at block <= 127 -- half the space the uint8_t
  // offset table suggests. The three-phase clean bank blew exactly this at
  // cool temperature ranges (fast start at block 131), which killed every
  // refresh the firmware makes and blanked the display below ~27 C while
  // warmer boots worked. Stubbing the two dead slots puts the fast start near
  // block 70 with room to grow.
  static constexpr uint32_t kUnusedLut[] = {0u};
  const size_t range = freeink::ed047tc2::tempRangeIndex(tempC);
  const uint32_t* fast = freeink::ed047tc2::kFastLut[range];
  const size_t fastStep = freeink::ed047tc2::kFastLutStep[range];
  const uint32_t* clean = freeink::ed047tc2::kCleanLut[range];
  const size_t cleanStep = freeink::ed047tc2::kCleanLutStep[range];
  // The grey columns of the fast bank are cut for these two levels in this range,
  // so the canvas has to address exactly them. Reading both from the same table
  // index is what keeps the two in step when the waveform is regenerated.
  const uint8_t grayDark = freeink::grayLevelByte(freeink::ed047tc2::kGrayLevelDark[range]);
  const uint8_t grayLight = freeink::grayLevelByte(freeink::ed047tc2::kGrayLevelLight[range]);

  // Worth a line on the console: the drive length is the single number that
  // decides how the panel looks, it is chosen once and never revisited, and a
  // thermistor that reads high silently under-drives every transition. Without
  // this there is no way to tell a waveform problem from a wrong temperature.
  Serial.printf("[epd] panel %d C%s -> waveform range %u (%u..%u C), %u drive frames, AA greys at level %u/%u\n",
                tempC, measuredOk ? "" : " (assumed, thermistor read failed)", static_cast<unsigned>(range),
                freeink::ed047tc2::kTempRanges[range].minC, freeink::ed047tc2::kTempRanges[range].maxC,
                freeink::ed047tc2::kDriveFrames[range], freeink::ed047tc2::kGrayLevelDark[range],
                freeink::ed047tc2::kGrayLevelLight[range]);

  // Each slot gets the waveform that matches how Panel_EPD drives it: the
  // differential modes get the vendor DU table extended with the two AA grey
  // columns, and the two modes that re-drive unchanged pixels get the charge
  // neutral clean refresh. Filling all four keeps LovyanGFX's generic LUTs,
  // which are tuned for a different panel, out of the picture entirely.
  return {
      {EP_D0, EP_D1, EP_D2, EP_D3, EP_D4, EP_D5, EP_D6, EP_D7},
      EP_STH,
      EP_STV,
      T5S3_LORA_CS,
      EP_LEH,
      EP_CKH,
      EP_CKV,
      T5S3_LORA_CS,
      16000000,
      8,
      0,
      {&prepareEpdPower, &epdPowerOn, &epdPowerOff},
      kUnusedLut,  // lutQuality — write-only mode here; never refreshed with
      1,
      clean,  // lutText   — Full/Half refreshes (the GC16-style blink)
      cleanStep,
      fast,  // lutFast   — page turns and the single-push AA
      fastStep,
      kUnusedLut,  // lutFastest — unused by this stack
      1,
      grayDark,
      grayLight,
      true,  // grey columns live in the fast bank above
  };
}

}  // namespace

namespace freeink {

const LgfxEpdConfig& lilygoT5S3LgfxConfig() {
  // Built once, on the first call, which is the driver's begin(): LovyanGFX
  // expands the LUTs into its own tables at panel init and never re-reads them,
  // so the temperature the waveform is chosen for is the temperature at boot.
  // The reader deep-sleeps between sessions and re-runs setup() on wake, so in
  // practice the waveform tracks the ambient temperature session by session.
  static const LgfxEpdConfig cfg = buildConfig();
  return cfg;
}

}  // namespace freeink
