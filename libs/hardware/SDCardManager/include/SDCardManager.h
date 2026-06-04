#pragma once

// FreeInk SDK — SD card manager (singleton). Two interchangeable backends behind
// one FsVolume& seam, so every op returns ordinary FsFile objects:
//   * SPI / SdFat (default) — SD power sequencing for PMIC-gated boards (e.g.
//     M5Stack PaperColor) is handled in begin().
//   * Native 4-bit SDMMC (FREEINK_SD_SDMMC, e.g. de-link) — SdFat can't drive
//     SDIO, so a plain FsVolume is mounted on an esp-idf SDMMC block device
//     (src/SdmmcBlockDevice). Requires the build to set USE_BLOCK_DEVICE_INTERFACE=1.
// The public API below is identical for both, so consumers are unchanged.

#include <WString.h>
#include <vector>
#include <string>
#include <SdFat.h>
#include <BoardConfig.h>

#if FREEINK_SD_SDMMC
namespace freeink {
class SdmmcBlockDevice;  // native esp-idf SDMMC block device (src/SdmmcBlockDevice.h)
}
#endif

class SDCardManager {
 public:
  SDCardManager();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  FsFile open(const char* path, const oflag_t oflag = O_RDONLY) { return vol().open(path, oflag); }
  bool mkdir(const char* path, const bool pFlag = true) { return vol().mkdir(path, pFlag); }
  bool exists(const char* path) { return vol().exists(path); }
  bool remove(const char* path) { return vol().remove(path); }
  bool rmdir(const char* path) { return vol().rmdir(path); }
  bool rename(const char* path, const char* newPath) { return vol().rename(path, newPath); }

  bool openFileForRead(const char* moduleName, const char* path, FsFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForRead(const char* moduleName, const String& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, FsFile& file);
  bool removeDir(const char* path);

 static SDCardManager& getInstance() { return instance; }

 private:
  static SDCardManager instance;

  bool initialized = false;

  // All filesystem ops route through one FsVolume& so the backend is swappable.
  // SPI boards: `sd` (SdFs is-a FsVolume). SDMMC boards: a bare FsVolume mounted
  // on a native esp-idf block device — both hand back ordinary FsFile objects.
#if FREEINK_SD_SDMMC
  FsVolume _vol;
  freeink::SdmmcBlockDevice* _dev = nullptr;  // owned, created in begin()
  FsVolume& vol() { return _vol; }
#else
  SdFat sd;
  FsVolume& vol() { return sd; }
#endif
};

#define SdMan SDCardManager::getInstance()
