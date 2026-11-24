#pragma once

#include <cassert>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
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
inline bool filesMode = false;
inline std::map<std::string, std::string> files;
inline unsigned mutation = 0, cutAfter = 0, failMutation = 0;
inline bool fileSyncFails = false, fileCloseFails = false;
inline size_t maxPrint = std::numeric_limits<size_t>::max();
inline unsigned openWriters = 0;
inline bool beforeMutation() { return ++mutation != failMutation; }
inline void afterMutation() {
  if (mutation == cutAfter) throw mutation;
}

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
inline std::vector<std::pair<std::string, std::string>> renameFailures;
inline std::vector<std::string> removeFailures;
}  // namespace fakeSd

class FsVolume;
class FsFile {
 public:
  explicit FsFile(bool open = false) : open_(open) {}
  FsFile(const char* path, bool writing) : open_(true), path_(path), writing_(writing) {
    if (writing_) ++fakeSd::openWriters;
  }
  explicit operator bool() const { return open_; }
  bool openRoot(FsVolume*) {
    root_ = true;
    open_ = !fakeSd::rootOpenFails;
    return open_;
  }
  bool sync() {
    if (fakeSd::filesMode && !root_) return !fakeSd::fileSyncFails;
    ++fakeSd::rootSyncs;
    return open_ && !fakeSd::rootSyncFails;
  }
  uint64_t fileSize() const { return fakeSd::filesMode ? fakeSd::files.at(path_).size() : fakeSd::advertisedSize; }
  bool isDirectory() const { return false; }
  bool close() {
    if (fakeSd::filesMode && !root_) {
      if (open_ && writing_) --fakeSd::openWriters;
      open_ = false;
      return !fakeSd::fileCloseFails;
    }
    if (open_) ++fakeSd::closes;
    open_ = false;
    return !(root_ && fakeSd::rootCloseFails);
  }
  int available() const { return cursor_ < (fakeSd::filesMode ? fileSize() : fakeSd::data.size()); }
  int read(void* destination, size_t count) {
    if (fakeSd::filesMode) {
      const auto& contents = fakeSd::files.at(path_);
      const size_t accepted = std::min(count, contents.size() - cursor_);
      std::memcpy(destination, contents.data() + cursor_, accepted);
      cursor_ += accepted;
      return static_cast<int>(accepted);
    }
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
  size_t getName(char*, size_t) const { return 0; }
  uint64_t size() const { return fileSize(); }
  size_t print(const String& text) {
    if (!fakeSd::filesMode) return text.size();
    if (!fakeSd::beforeMutation()) return 0;
    const size_t count = std::min(text.size(), fakeSd::maxPrint);
    fakeSd::files[path_].append(text.data(), count);
    fakeSd::afterMutation();
    return count;
  }

 private:
  bool open_;
  std::string path_;
  bool writing_ = false;
  size_t cursor_ = 0;
  bool root_ = false;
};

class FsVolume {
 public:
  bool begin(FsBlockDeviceInterface*) { return true; }
  void end() { ++fakeSd::volumeEnds; }
  FsFile open(const char* path, oflag_t flags = O_RDONLY) {
    if (!fakeSd::filesMode) return FsFile(!fakeSd::openFails);
    if (flags & O_TRUNC) {
      if (!fakeSd::beforeMutation()) return FsFile();
      fakeSd::files[path].clear();
      fakeSd::afterMutation();
    }
    if (!fakeSd::files.count(path)) return FsFile();
    return FsFile(path, flags != O_RDONLY);
  }
  bool exists(const char* path) const { return fakeSd::filesMode && fakeSd::files.count(path); }
  bool mkdir(const char*, bool = true) { return true; }
  bool remove(const char* path) {
    if (!fakeSd::filesMode) return true;
    assert(fakeSd::openWriters == 0);
    if (!fakeSd::beforeMutation()) return false;
    if (std::find(fakeSd::removeFailures.begin(), fakeSd::removeFailures.end(), path) != fakeSd::removeFailures.end()) {
      return false;
    }
    const bool result = fakeSd::files.erase(path) != 0;
    fakeSd::afterMutation();
    return result;
  }
  bool rmdir(const char*) { return true; }
  bool rename(const char* from, const char* to) {
    if (!fakeSd::filesMode) return true;
    assert(fakeSd::openWriters == 0);
    if (!fakeSd::beforeMutation()) return false;
    if (std::find(fakeSd::renameFailures.begin(), fakeSd::renameFailures.end(),
                  std::make_pair(std::string(from), std::string(to))) != fakeSd::renameFailures.end()) {
      return false;
    }
    if (!fakeSd::files.count(from) || fakeSd::files.count(to)) return false;
    fakeSd::files[to] = fakeSd::files[from];
    fakeSd::files.erase(from);
    fakeSd::afterMutation();
    return true;
  }
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
