#pragma once

// FreeInk SDK — SD card manager (singleton). Device-agnostic: it knows no board
// names. Two interchangeable backends behind one FsVolume& seam, so every op
// returns ordinary FsFile objects:
//   * SPI / SdFat (default).
//   * Native 4-bit SDMMC (FREEINK_SD_SDMMC, e.g. de-link) — SdFat can't drive
//     SDIO, so a plain FsVolume is mounted on an esp-idf SDMMC block device
//     (src/SdmmcBlockDevice). Requires the build to set USE_BLOCK_DEVICE_INTERFACE=1.
// Boards whose SD rail needs more than a GPIO (e.g. an I2C PMIC) register their
// power-up via setPowerHook(); the manager calls it but stays device-agnostic.
// The public API is identical for both backends, so consumers are unchanged.

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
  // Returns the total card capacity in bytes. Cached at begin(); 0 if not mounted.
  uint64_t sdTotalBytes() const;
  // Returns used space in bytes, cached with a 20-second TTL (freeClusterCount
  // scans the FAT and is too slow to call on every frame). 0 if not mounted or
  // the cluster count cannot be determined.
  uint64_t sdUsedBytes();
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

  // Optional board hook to bring up SD-card power before the card is mounted, for
  // boards whose SD rail isn't a plain GPIO (e.g. behind an I2C PMIC). Called once
  // at the start of begin(). The board registers it from its own board-support
  // layer; the SD manager itself stays device-agnostic. Default: none.
  using PowerHook = void (*)();
  void setPowerHook(PowerHook hook) { _powerHook = hook; }

 static SDCardManager& getInstance() { return instance; }

 private:
  static SDCardManager instance;

  bool initialized = false;
  PowerHook _powerHook = nullptr;

  static constexpr uint32_t USED_BYTES_CACHE_TTL_MS = 20000;
  uint64_t cachedTotalBytes = 0;
  uint64_t cachedUsedBytes = 0;
  uint32_t cachedUsedBytesAt = 0;
  bool cachedUsedBytesValid = false;

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
