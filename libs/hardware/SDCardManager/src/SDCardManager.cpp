#include "SDCardManager.h"

#include <BoardConfig.h>
#include <SPI.h>

#include "SdmmcBlockDevice.h"  // no-op unless FREEINK_SD_SDMMC

SDCardManager SDCardManager::instance;

#if FREEINK_SD_SDMMC
SDCardManager::SDCardManager() {}

bool SDCardManager::begin() {
  // Native 4-bit SDMMC: SdFat can't drive SDIO, so mount a plain FsVolume on the
  // esp-idf SDMMC block device. FsFile from this volume is the same type the SPI
  // path returns, so the public API and consumers are unchanged.
  if (_powerHook) _powerHook();  // board brings up its SD rail (e.g. PMIC) if needed
  if (!_dev) _dev = new freeink::SdmmcBlockDevice();
  if (!_dev->begin(BoardConfig::ACTIVE.sdmmc)) {
    if (Serial) Serial.printf("[%lu] [SD] SDMMC init failed\n", millis());
    initialized = false;
    return false;
  }
  if (!_vol.begin(_dev)) {
    if (Serial) Serial.printf("[%lu] [SD] SDMMC volume mount failed\n", millis());
    initialized = false;
    return false;
  }
  if (Serial) Serial.printf("[%lu] [SD] SDMMC card mounted\n", millis());
  initialized = true;
  return initialized;
}
#else
SDCardManager::SDCardManager() : sd() {}

bool SDCardManager::begin() {
  // Pins/clock come from the runtime-active profile (board-overridable via
  // BoardConfig::ACTIVE.sd.spiHz; 0 = default). Read after device selection.
  const uint8_t SD_CS = BoardConfig::ACTIVE.sd.cs;
  const uint32_t SPI_FQ = BoardConfig::ACTIVE.sd.spiHz != 0 ? BoardConfig::ACTIVE.sd.spiHz : 40000000;

  if (_powerHook) _powerHook();  // board brings up its SD rail (e.g. PMIC) if needed

  // Boards that gate the SD rail with a plain GPIO (e.g. Sticky's SD_PWR_EN on
  // GPIO10) must power it before probing. Active-high enable + a brief settle.
  // No-op when unassigned, and complements _powerHook for PMIC-gated boards.
  if (BoardConfig::ACTIVE.sd.powerEnable >= 0) {
    pinMode(BoardConfig::ACTIVE.sd.powerEnable, OUTPUT);
    digitalWrite(BoardConfig::ACTIVE.sd.powerEnable, HIGH);
    delay(10);
  }

  // Shared SPI bus: when the display controller sits on the same SCLK as the SD
  // card (e.g. M5Paper's IT8951 on 14/12/13), deselect it (CS high) before
  // probing the card. SD init runs before the display driver's begin(), so a
  // powered, never-deselected panel can drive the shared MISO and break
  // detection. Harmless when the display is on a separate bus or has no CS pin.
  if (BoardConfig::ACTIVE.display.cs >= 0 && BoardConfig::ACTIVE.display.sclk == BoardConfig::ACTIVE.sd.sclk) {
    pinMode(BoardConfig::ACTIVE.display.cs, OUTPUT);
    digitalWrite(BoardConfig::ACTIVE.display.cs, HIGH);
  }

  if (BoardConfig::ACTIVE.sd.sclk >= 0 && BoardConfig::ACTIVE.sd.mosi >= 0 && BoardConfig::ACTIVE.sd.miso >= 0) {
    SPI.begin(BoardConfig::ACTIVE.sd.sclk, BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.sd.mosi, SD_CS);
  }

  if (!sd.begin(SD_CS, SPI_FQ)) {
    if (Serial)
      Serial.printf("[%lu] [SD] SD card not detected (err=0x%02X data=0x%02X cs=%d sclk=%d miso=%d mosi=%d clk=%luHz)\n",
                    millis(), sd.sdErrorCode(), sd.sdErrorData(), SD_CS, BoardConfig::ACTIVE.sd.sclk,
                    BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.sd.mosi, (unsigned long)SPI_FQ);
    initialized = false;
  } else {
    if (Serial) Serial.printf("[%lu] [SD] SD card detected\n", millis());
    initialized = true;
  }

  return initialized;
}
#endif

bool SDCardManager::ready() const {
  return initialized;
}

std::vector<String> SDCardManager::listFiles(const char* path, const int maxFiles) {
  std::vector<String> ret;
  if (!initialized) {
    if (Serial) Serial.printf("[%lu] [SD] not initialized, returning empty list\n", millis());
    return ret;
  }

  auto root = vol().open(path);
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
  if (vol().exists(path)) {
    vol().remove(path);
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
  if (vol().exists(path)) {
    FsFile dir = vol().open(path);
    if (dir && dir.isDirectory()) {
      dir.close();
      return true;
    }
    dir.close();
  }

  // Create the directory
  if (vol().mkdir(path)) {
    return true;
  }
  if (Serial) Serial.printf("Failed to create directory: %s\n", path);
  return false;
}

bool SDCardManager::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
  if (!vol().exists(path)) {
    if (Serial) Serial.printf("[%lu] [%s] File does not exist: %s\n", millis(), moduleName, path);
    return false;
  }

  file = vol().open(path, O_RDONLY);
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
  file = vol().open(path, O_RDWR | O_CREAT | O_TRUNC);
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
  auto dir = vol().open(path);
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
      if (!vol().remove(filePath.c_str())) {
        return false;
      }
    }
    file = dir.openNextFile();
  }

  return vol().rmdir(path);
}
