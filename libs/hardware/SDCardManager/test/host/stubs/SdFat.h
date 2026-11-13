#pragma once

#include <cstring>
#include <limits>
#include <vector>

#include "WString.h"

using oflag_t = int;
constexpr oflag_t O_RDONLY = 0, O_RDWR = 1, O_CREAT = 2, O_TRUNC = 4;
using Sector_t = uint32_t;
class FsBlockDeviceInterface {
 public:
  virtual ~FsBlockDeviceInterface() = default;
  virtual void end() {}
  virtual bool isBusy() { return false; }
  virtual bool readSector(Sector_t, uint8_t*) { return false; }
  virtual bool readSectors(Sector_t, uint8_t*, size_t) { return false; }
  virtual bool writeSector(Sector_t, const uint8_t*) { return false; }
  virtual bool writeSectors(Sector_t, const uint8_t*, size_t) { return false; }
  virtual Sector_t sectorCount() { return 1024; }
  virtual bool syncDevice() { return true; }
};

namespace fakeSd {
inline std::vector<uint8_t> data;
inline uint64_t advertisedSize = 0;
inline bool openFails = false;
inline size_t failAt = std::numeric_limits<size_t>::max();
inline size_t maxRead = std::numeric_limits<size_t>::max();
inline size_t largestRequest = 0;
inline unsigned reads = 0, closes = 0;
inline uint32_t readMs = 0;
inline int32_t freeClusters = 1;
inline uint32_t clusters = 1;
inline unsigned clusterScans = 0;
inline bool rootOpenFails = false, rootSyncFails = false, rootCloseFails = false;
inline unsigned rootSyncs = 0, volumeEnds = 0;
}  // namespace fakeSd

class FsVolume;
class FsFile {
 public:
  explicit FsFile(bool open = false) : open_(open) {}
  explicit operator bool() const { return open_; }
  bool openRoot(FsVolume*) {
    root_ = true;
    open_ = !fakeSd::rootOpenFails;
    return open_;
  }
  bool sync() {
    ++fakeSd::rootSyncs;
    return open_ && !fakeSd::rootSyncFails;
  }
  uint64_t fileSize() const { return fakeSd::advertisedSize; }
  bool isDirectory() const { return false; }
  bool close() {
    if (open_) ++fakeSd::closes;
    open_ = false;
    return !(root_ && fakeSd::rootCloseFails);
  }
  int available() const { return cursor_ < fakeSd::data.size(); }
  int read(void* destination, size_t count) {
    ++fakeSd::reads;
    nowMs += fakeSd::readMs;
    fakeSd::largestRequest = std::max(fakeSd::largestRequest, count);
    if (cursor_ >= fakeSd::failAt) return -1;
    const size_t accepted = std::min({count, fakeSd::data.size() - cursor_, fakeSd::maxRead});
    if (accepted != 0) std::memcpy(destination, fakeSd::data.data() + cursor_, accepted);
    cursor_ += accepted;
    return static_cast<int>(accepted);
  }
  int read() {
    uint8_t byte;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  FsFile openNextFile() { return FsFile(); }
  void getName(char*, size_t) const {}
  size_t print(const String& text) { return text.size(); }

 private:
  bool open_;
  size_t cursor_ = 0;
  bool root_ = false;
};

class FsVolume {
 public:
  bool begin(FsBlockDeviceInterface*) { return true; }
  void end() { ++fakeSd::volumeEnds; }
  FsFile open(const char*, oflag_t = O_RDONLY) { return FsFile(!fakeSd::openFails); }
  bool exists(const char*) const { return false; }
  bool mkdir(const char*, bool = true) { return true; }
  bool remove(const char*) { return true; }
  bool rmdir(const char*) { return true; }
  bool rename(const char*, const char*) { return true; }
  uint32_t clusterCount() const { return fakeSd::clusters; }
  uint32_t bytesPerCluster() const { return 512; }
  int32_t freeClusterCount() const {
    ++fakeSd::clusterScans;
    return fakeSd::freeClusters;
  }
};

class SdFat : public FsVolume {
 public:
  FsBlockDeviceInterface* card() { return &card_; }
  bool begin(uint8_t, uint32_t) { return true; }
  int sdErrorCode() const { return 0; }
  int sdErrorData() const { return 0; }

 private:
  FsBlockDeviceInterface card_;
};
