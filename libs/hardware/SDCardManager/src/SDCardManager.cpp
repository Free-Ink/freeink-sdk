#include "SDCardManager.h"

#include <BoardConfig.h>
#include <SPI.h>
#include <Wire.h>

namespace {
bool writeI2CRegister8(const uint8_t addr, const uint8_t reg, const uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool updateI2CRegister8(const uint8_t addr, const uint8_t reg, const uint8_t clearMask, const uint8_t setMask) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) {
    return false;
  }
  const uint8_t current = Wire.read();
  return writeI2CRegister8(addr, reg, static_cast<uint8_t>((current & ~clearMask) | setMask));
}

void enableM5PaperColorSdPower() {
#if defined(CROSSPOINT_BOARD_M5STACK_PAPERCOLOR) || defined(BOARD_M5STACK_PAPERCOLOR)
  constexpr uint8_t PMIC_ADDR = 0x6E;
  constexpr uint32_t I2C_FREQ = 100000;
  constexpr uint8_t PMIC_GPIO3 = 1 << 3;

  Wire.begin(3, 2, I2C_FREQ);
  Wire.setTimeOut(20);

  // M5PaperColor uses M5PM1 GPIO3 as SD-card power enable.
  // REG 0x16: GPIO function select, 0 = GPIO.
  // REG 0x10: GPIO direction, 1 = output.
  // REG 0x13: GPIO output type, 0 = push-pull.
  // REG 0x11: GPIO output value, 1 = high.
  writeI2CRegister8(PMIC_ADDR, 0x09, 0x00);
  updateI2CRegister8(PMIC_ADDR, 0x16, PMIC_GPIO3, 0);
  updateI2CRegister8(PMIC_ADDR, 0x10, 0, PMIC_GPIO3);
  updateI2CRegister8(PMIC_ADDR, 0x13, PMIC_GPIO3, 0);
  updateI2CRegister8(PMIC_ADDR, 0x11, 0, PMIC_GPIO3);
  delay(100);
#endif
}
}  // namespace

SDCardManager SDCardManager::instance;

SDCardManager::SDCardManager() : sd() {}

bool SDCardManager::begin() {
  // Pins/clock come from the runtime-active profile (board-overridable via
  // BoardConfig::ACTIVE.sd.spiHz; 0 = default). Read after device selection.
  const uint8_t SD_CS = BoardConfig::ACTIVE.sd.cs;
  const uint32_t SPI_FQ = BoardConfig::ACTIVE.sd.spiHz != 0 ? BoardConfig::ACTIVE.sd.spiHz : 40000000;

  if (BoardConfig::isM5StackPaperColor()) {
    enableM5PaperColorSdPower();
  }

  if (BoardConfig::ACTIVE.sd.sclk >= 0 && BoardConfig::ACTIVE.sd.mosi >= 0 && BoardConfig::ACTIVE.sd.miso >= 0) {
    SPI.begin(BoardConfig::ACTIVE.sd.sclk, BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.sd.mosi, SD_CS);
  }

  if (!sd.begin(SD_CS, SPI_FQ)) {
    if (Serial) Serial.printf("[%lu] [SD] SD card not detected\n", millis());
    initialized = false;
  } else {
    if (Serial) Serial.printf("[%lu] [SD] SD card detected\n", millis());
    initialized = true;
  }

  return initialized;
}

bool SDCardManager::ready() const {
  return initialized;
}

std::vector<String> SDCardManager::listFiles(const char* path, const int maxFiles) {
  std::vector<String> ret;
  if (!initialized) {
    if (Serial) Serial.printf("[%lu] [SD] not initialized, returning empty list\n", millis());
    return ret;
  }

  auto root = sd.open(path);
  if (!root) {
    if (Serial) Serial.printf("[%lu] [SD] Failed to open directory\n", millis());
    return ret;
  }
  if (!root.isDirectory()) {
    if (Serial) Serial.printf("[%lu] [SD] Path is not a directory\n", millis());
    root.close();
    return ret;
  }

  int count = 0;
  char name[128];
  for (auto f = root.openNextFile(); f && count < maxFiles; f = root.openNextFile()) {
    if (f.isDirectory()) {
      f.close();
      continue;
    }
    f.getName(name, sizeof(name));
    ret.emplace_back(name);
    f.close();
    count++;
  }
  root.close();
  return ret;
}

String SDCardManager::readFile(const char* path) {
  if (!initialized) {
    if (Serial) Serial.printf("[%lu] [SD] not initialized; cannot read file\n", millis());
    return {""};
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return {""};
  }

  String content = "";
  constexpr size_t maxSize = 50000;  // Limit to 50KB
  size_t readSize = 0;
  while (f.available() && readSize < maxSize) {
    const char c = static_cast<char>(f.read());
    content += c;
    readSize++;
  }
  f.close();
  return content;
}

bool SDCardManager::readFileToStream(const char* path, Print& out, const size_t chunkSize) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot read file");
    return false;
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return false;
  }

  constexpr size_t localBufSize = 256;
  uint8_t buf[localBufSize];
  const size_t toRead = (chunkSize == 0) ? localBufSize : (chunkSize < localBufSize ? chunkSize : localBufSize);

  while (f.available()) {
    const int r = f.read(buf, toRead);
    if (r > 0) {
      out.write(buf, static_cast<size_t>(r));
    } else {
      break;
    }
  }

  f.close();
  return true;
}

size_t SDCardManager::readFileToBuffer(const char* path, char* buffer, const size_t bufferSize, const size_t maxBytes) {
  if (!buffer || bufferSize == 0)
    return 0;
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot read file");
    buffer[0] = '\0';
    return 0;
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    buffer[0] = '\0';
    return 0;
  }

  const size_t maxToRead = (maxBytes == 0) ? (bufferSize - 1) : min(maxBytes, bufferSize - 1);
  size_t total = 0;

  while (f.available() && total < maxToRead) {
    constexpr size_t chunk = 64;
    const size_t want = maxToRead - total;
    const size_t readLen = (want < chunk) ? want : chunk;
    const int r = f.read(buffer + total, readLen);
    if (r > 0) {
      total += static_cast<size_t>(r);
    } else {
      break;
    }
  }

  buffer[total] = '\0';
  f.close();
  return total;
}

bool SDCardManager::writeFile(const char* path, const String& content) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot write file");
    return false;
  }

  // Remove existing file so we perform an overwrite rather than append
  if (sd.exists(path)) {
    sd.remove(path);
  }

  FsFile f;
  if (!openFileForWrite("SD", path, f)) {
    if (Serial) Serial.printf("Failed to open file for write: %s\n", path);
    return false;
  }

  const size_t written = f.print(content);
  f.close();
  return written == content.length();
}

bool SDCardManager::ensureDirectoryExists(const char* path) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot create directory");
    return false;
  }

  // Check if directory already exists
  if (sd.exists(path)) {
    FsFile dir = sd.open(path);
    if (dir && dir.isDirectory()) {
      dir.close();
      return true;
    }
    dir.close();
  }

  // Create the directory
  if (sd.mkdir(path)) {
    return true;
  }
  if (Serial) Serial.printf("Failed to create directory: %s\n", path);
  return false;
}

bool SDCardManager::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
  if (!sd.exists(path)) {
    if (Serial) Serial.printf("[%lu] [%s] File does not exist: %s\n", millis(), moduleName, path);
    return false;
  }

  file = sd.open(path, O_RDONLY);
  if (!file) {
    if (Serial) Serial.printf("[%lu] [%s] Failed to open file for reading: %s\n", millis(), moduleName, path);
    return false;
  }
  return true;
}

bool SDCardManager::openFileForRead(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForRead(const char* moduleName, const String& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const char* path, FsFile& file) {
  file = sd.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    if (Serial) Serial.printf("[%lu] [%s] Failed to open file for writing: %s\n", millis(), moduleName, path);
    return false;
  }
  return true;
}

bool SDCardManager::openFileForWrite(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const String& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::removeDir(const char* path) {
  // 1. Open the directory
  auto dir = sd.open(path);
  if (!dir) {
    return false;
  }
  if (!dir.isDirectory()) {
    return false;
  }

  auto file = dir.openNextFile();
  char name[128];
  while (file) {
    String filePath = path;
    if (!filePath.endsWith("/")) {
      filePath += "/";
    }
    file.getName(name, sizeof(name));
    filePath += name;

    if (file.isDirectory()) {
      if (!removeDir(filePath.c_str())) {
        return false;
      }
    } else {
      if (!sd.remove(filePath.c_str())) {
        return false;
      }
    }
    file = dir.openNextFile();
  }

  return sd.rmdir(path);
}
