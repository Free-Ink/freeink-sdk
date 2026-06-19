#include <BoardT5S3.h>
#include <LgfxEpdConfig.h>
#include <Wire.h>

namespace {

constexpr int kDefaultVcomMv = -1600;
constexpr uint8_t kTpsRegEnable = 0x01;
constexpr uint8_t kTpsRegVcom = 0x03;
constexpr uint8_t kTpsRegPowerGood = 0x0F;
constexpr uint8_t kTpsEnableOutputs = 0x3F;

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

}  // namespace

namespace freeink {

const LgfxEpdConfig& lilygoT5S3LgfxConfig() {
  static const LgfxEpdConfig cfg = {
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
  };
  return cfg;
}

}  // namespace freeink
