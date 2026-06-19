#include <BoardT5S3.h>

#include <InputManager.h>
#include <SPI.h>
#include <Wire.h>
#include <cassert>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace BoardT5S3 {
namespace {
constexpr uint8_t PCA_REG_INPUT0 = 0x00;
constexpr uint8_t PCA_REG_OUTPUT0 = 0x02;
constexpr uint8_t PCA_REG_CONFIG0 = 0x06;

SemaphoreHandle_t i2cMutex = nullptr;

SemaphoreHandle_t ensureI2CMutex() {
  if (i2cMutex == nullptr) {
    i2cMutex = xSemaphoreCreateRecursiveMutex();
    assert(i2cMutex != nullptr && "Failed to create I2C mutex");
  }
  return i2cMutex;
}

bool i2cWriteReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len) {
  ScopedI2CLock lock;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (data != nullptr && len > 0) {
    Wire.write(data, len);
  }
  return Wire.endTransmission() == 0;
}

bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t* data, size_t len) {
  ScopedI2CLock lock;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  const uint8_t requested = static_cast<uint8_t>(len);
  if (Wire.requestFrom(addr, requested) != requested) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

bool updatePca9535Bit(uint8_t baseReg, uint8_t pin, bool high) {
  const uint8_t port = pin / 8;
  const uint8_t bit = pin % 8;
  uint8_t value = 0;
  if (!i2cReadReg(T5S3_PCA9535_ADDR, baseReg + port, &value, 1)) {
    return false;
  }
  if (high) {
    value |= static_cast<uint8_t>(1U << bit);
  } else {
    value &= static_cast<uint8_t>(~(1U << bit));
  }
  return i2cWriteReg(T5S3_PCA9535_ADDR, baseReg + port, &value, 1);
}

uint8_t inputButtonHook() { return readButton() ? static_cast<uint8_t>(1U << InputManager::BTN_DOWN) : 0; }
}  // namespace

ScopedI2CLock::ScopedI2CLock() {
  xSemaphoreTakeRecursive(ensureI2CMutex(), portMAX_DELAY);
  locked_ = true;
}

ScopedI2CLock::~ScopedI2CLock() {
  if (locked_) {
    xSemaphoreGiveRecursive(ensureI2CMutex());
    locked_ = false;
  }
}

void beginI2C() {
  ensureI2CMutex();
  Wire.begin(T5S3_SDA, T5S3_SCL);
  Wire.setClock(T5S3_I2C_FREQ);
  Wire.setTimeOut(50);
}

void prepareSdBus() {
  pinMode(T5S3_LORA_CS, OUTPUT);
  digitalWrite(T5S3_LORA_CS, HIGH);
  pinMode(T5S3_SD_CS, OUTPUT);
  digitalWrite(T5S3_SD_CS, HIGH);
  SPI.begin(T5S3_SPI_SCLK, T5S3_SPI_MISO, T5S3_SPI_MOSI, T5S3_SD_CS);
}

void disableGpsLora() {
  pinMode(T5S3_LORA_CS, OUTPUT);
  digitalWrite(T5S3_LORA_CS, HIGH);
  pinMode(T5S3_LORA_RST, OUTPUT);
  digitalWrite(T5S3_LORA_RST, LOW);
  pinMode(T5S3_LORA_IRQ, INPUT);
  pinMode(T5S3_LORA_BUSY, INPUT);
  pinMode(T5S3_GPS_RXD, INPUT);
  pinMode(T5S3_GPS_TXD, INPUT);

  writePca9535Pin(PCA9535_IO00_LORA_GPS_EN, false);
  setPca9535PinMode(PCA9535_IO00_LORA_GPS_EN, OUTPUT);
}

void begin() {
  beginI2C();
  pinMode(T5S3_BOOT_BTN, INPUT_PULLUP);
  if (T5S3_PCA9535_INT > 0) {
    pinMode(T5S3_PCA9535_INT, INPUT_PULLUP);
  }

  prepareSdBus();
  disableGpsLora();
  setPca9535PinMode(PCA9535_IO12_BUTTON, INPUT);
  InputManager::setButtonHook(inputButtonHook);
}

bool pca9535Present() {
  ScopedI2CLock lock;
  Wire.beginTransmission(T5S3_PCA9535_ADDR);
  return Wire.endTransmission() == 0;
}

bool setPca9535PinMode(uint8_t pin, uint8_t mode) {
  const bool inputMode = mode != OUTPUT;
  return updatePca9535Bit(PCA_REG_CONFIG0, pin, inputMode);
}

bool writePca9535Pin(uint8_t pin, bool high) { return updatePca9535Bit(PCA_REG_OUTPUT0, pin, high); }

bool readPca9535Pin(uint8_t pin, bool* high) {
  if (!high) {
    return false;
  }
  const uint8_t port = pin / 8;
  const uint8_t bit = pin % 8;
  uint8_t value = 0;
  if (!i2cReadReg(T5S3_PCA9535_ADDR, PCA_REG_INPUT0 + port, &value, 1)) {
    return false;
  }
  *high = (value & (1U << bit)) != 0;
  return true;
}

bool readButton() {
  bool high = true;
  if (!readPca9535Pin(PCA9535_IO12_BUTTON, &high)) {
    return false;
  }
  return !high;
}

}  // namespace BoardT5S3
