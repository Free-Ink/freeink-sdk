// FreeInkBook — ZIP central-directory catalog and streaming entry reader.

#include "epub/ZipCatalog.h"

#include <string.h>

#include "epub/MinizConfig.h"

namespace freeink {
namespace book {

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;   // end of central directory
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;
constexpr size_t kEocdMinSize = 22;
constexpr size_t kMaxCommentScan = 65535 + kEocdMinSize;
constexpr size_t kMaxNameLen = 1024;
constexpr uint32_t kInBufSize = 4096;
constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflate = 8;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Buffered forward reader over a BookSource, used to walk the central
// directory without one readAt() per field.
class SeqReader {
 public:
  SeqReader(BookSource& source, uint64_t offset) : source_(source), pos_(offset) {}

  bool read(void* dst, uint32_t len) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    while (len > 0) {
      if (avail_ == 0 && !refill()) return false;
      const uint32_t take = avail_ < len ? avail_ : len;
      memcpy(out, buf_ + bufPos_, take);
      out += take;
      bufPos_ += take;
      avail_ -= take;
      len -= take;
    }
    return true;
  }

  void skip(uint32_t len) {
    const uint32_t buffered = avail_ < len ? avail_ : len;
    bufPos_ += buffered;
    avail_ -= buffered;
    pos_ += len - buffered;
  }

 private:
  bool refill() {
    const int32_t n = source_.readAt(pos_, buf_, sizeof(buf_));
    if (n <= 0) return false;
    avail_ = static_cast<uint32_t>(n);
    bufPos_ = 0;
    pos_ += static_cast<uint32_t>(n);
    return true;
  }

  BookSource& source_;
  uint64_t pos_;
  uint8_t buf_[1024];
  uint32_t bufPos_ = 0;
  uint32_t avail_ = 0;
};

}  // namespace

uint32_t ZipCatalog::hashPath(const char* path) {
  uint32_t hash = 2166136261u;  // FNV-1a
  while (*path != '\0') {
    hash ^= static_cast<uint8_t>(*path++);
    hash *= 16777619u;
  }
  return hash;
}

BookStatus ZipCatalog::open(BookSource& source, Arena& arena) {
  source_ = &source;
  entries_ = nullptr;
  count_ = 0;

  const uint64_t fileSize = source.size();
  if (fileSize < kEocdMinSize) return BookStatus::NotZip;

  // Find the end-of-central-directory record: scan backward from the end of
  // the file (the ZIP comment can push it up to 64 KB in) using a small
  // stack window with 3 bytes of overlap so the signature can straddle reads.
  const uint64_t maxBack = fileSize < kMaxCommentScan ? fileSize : kMaxCommentScan;
  uint64_t eocdPos = UINT64_MAX;
  uint64_t lowest = fileSize;  // lowest file offset scanned so far
  uint8_t window[4096 + 3];
  while (eocdPos == UINT64_MAX && (fileSize - lowest) < maxBack) {
    const uint64_t remain = maxBack - (fileSize - lowest);
    const uint32_t chunk = remain < 4096 ? static_cast<uint32_t>(remain) : 4096;
    const uint64_t off = lowest - chunk;
    const uint32_t overlap = (fileSize - lowest) < 3 ? static_cast<uint32_t>(fileSize - lowest) : 3;
    const uint32_t len = chunk + overlap;
    if (source.readAt(off, window, len) != static_cast<int32_t>(len)) return BookStatus::IoError;
    for (int64_t i = static_cast<int64_t>(len) - 4; i >= 0; --i) {
      if (le32(window + i) == kEocdSig) {
        eocdPos = off + static_cast<uint64_t>(i);
        break;
      }
    }
    lowest = off;
  }
  if (eocdPos == UINT64_MAX || eocdPos + kEocdMinSize > fileSize) return BookStatus::NotZip;

  uint8_t eocd[kEocdMinSize];
  if (source.readAt(eocdPos, eocd, kEocdMinSize) != static_cast<int32_t>(kEocdMinSize)) {
    return BookStatus::IoError;
  }
  const uint16_t totalEntries = le16(eocd + 10);
  const uint32_t dirSize = le32(eocd + 12);
  const uint32_t dirOffset = le32(eocd + 16);
  if (totalEntries == 0xFFFF || dirOffset == 0xFFFFFFFFu || dirSize == 0xFFFFFFFFu) {
    return BookStatus::Unsupported;  // zip64
  }
  if (static_cast<uint64_t>(dirOffset) + dirSize > fileSize) return BookStatus::Truncated;

  ZipEntry* entries = arena.allocArray<ZipEntry>(totalEntries);
  if (entries == nullptr && totalEntries != 0) return BookStatus::OutOfMemory;

  SeqReader reader(source, dirOffset);
  for (uint16_t i = 0; i < totalEntries; ++i) {
    uint8_t hdr[46];
    if (!reader.read(hdr, sizeof(hdr))) return BookStatus::Truncated;
    if (le32(hdr) != kCentralSig) return BookStatus::NotZip;
    const uint16_t method = le16(hdr + 10);
    const uint32_t compressedSize = le32(hdr + 20);
    const uint32_t uncompressedSize = le32(hdr + 24);
    const uint16_t nameLen = le16(hdr + 28);
    const uint16_t extraLen = le16(hdr + 30);
    const uint16_t commentLen = le16(hdr + 32);
    const uint32_t localOffset = le32(hdr + 42);
    if (nameLen == 0 || nameLen > kMaxNameLen) return BookStatus::Unsupported;

    char* name = static_cast<char*>(arena.alloc(static_cast<size_t>(nameLen) + 1, 1));
    if (name == nullptr) return BookStatus::OutOfMemory;
    if (!reader.read(name, nameLen)) return BookStatus::Truncated;
    name[nameLen] = '\0';
    reader.skip(static_cast<uint32_t>(extraLen) + commentLen);

    ZipEntry& entry = entries[i];
    entry.name = name;
    entry.nameHash = hashPath(name);
    entry.method = method;
    entry.compressedSize = compressedSize;
    entry.uncompressedSize = uncompressedSize;
    entry.localHeaderOffset = localOffset;
  }

  entries_ = entries;
  count_ = totalEntries;
  return BookStatus::Ok;
}

const ZipEntry* ZipCatalog::find(const char* path) const {
  if (path == nullptr) return nullptr;
  const uint32_t hash = hashPath(path);
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].nameHash == hash && strcmp(entries_[i].name, path) == 0) {
      return &entries_[i];
    }
  }
  return nullptr;
}

BookStatus ZipEntryReader::open(BookSource& source, const ZipEntry& entry, Arena& scratch) {
  source_ = &source;
  entry_ = &entry;
  produced_ = 0;
  inPos_ = inAvail_ = compConsumed_ = 0;
  windowPos_ = pendingPos_ = pendingLen_ = 0;
  done_ = false;
  decompressor_ = nullptr;
  window_ = nullptr;
  inBuf_ = nullptr;

  // The central directory and the local header may disagree on name/extra
  // lengths (some writers add extra fields locally), so the local header is
  // authoritative for where the data starts.
  uint8_t local[30];
  if (source.readAt(entry.localHeaderOffset, local, sizeof(local)) !=
      static_cast<int32_t>(sizeof(local))) {
    return BookStatus::IoError;
  }
  if (le32(local) != kLocalSig) return BookStatus::NotZip;
  const uint16_t nameLen = le16(local + 26);
  const uint16_t extraLen = le16(local + 28);
  dataOffset_ = static_cast<uint64_t>(entry.localHeaderOffset) + sizeof(local) + nameLen + extraLen;

  if (entry.method == kMethodStored) {
    if (entry.compressedSize != entry.uncompressedSize) return BookStatus::Truncated;
    return BookStatus::Ok;
  }
  if (entry.method != kMethodDeflate) return BookStatus::Unsupported;

  decompressor_ = scratch.alloc(sizeof(tinfl_decompressor), alignof(uint64_t));
  window_ = static_cast<uint8_t*>(scratch.alloc(TINFL_LZ_DICT_SIZE, 16));
  inBuf_ = static_cast<uint8_t*>(scratch.alloc(kInBufSize, 16));
  if (decompressor_ == nullptr || window_ == nullptr || inBuf_ == nullptr) {
    return BookStatus::OutOfMemory;
  }
  tinfl_init(static_cast<tinfl_decompressor*>(decompressor_));
  return BookStatus::Ok;
}

int32_t ZipEntryReader::read(void* dst, uint32_t len) {
  if (source_ == nullptr || entry_ == nullptr) return -1;
  if (len == 0) return 0;
  return entry_->method == kMethodStored ? readStored(static_cast<uint8_t*>(dst), len)
                                         : readDeflated(static_cast<uint8_t*>(dst), len);
}

int32_t ZipEntryReader::readStored(uint8_t* dst, uint32_t len) {
  const uint32_t remaining = entry_->uncompressedSize - produced_;
  const uint32_t want = remaining < len ? remaining : len;
  if (want == 0) return 0;
  const int32_t n = source_->readAt(dataOffset_ + produced_, dst, want);
  if (n <= 0) return -1;
  produced_ += static_cast<uint32_t>(n);
  return n;
}

int32_t ZipEntryReader::readDeflated(uint8_t* dst, uint32_t len) {
  tinfl_decompressor* decomp = static_cast<tinfl_decompressor*>(decompressor_);
  uint32_t out = 0;
  while (out < len) {
    if (pendingLen_ > 0) {
      const uint32_t take = pendingLen_ < (len - out) ? pendingLen_ : (len - out);
      memcpy(dst + out, window_ + pendingPos_, take);
      pendingPos_ += take;
      pendingLen_ -= take;
      out += take;
      continue;
    }
    if (done_) break;

    if (inAvail_ == 0 && compConsumed_ < entry_->compressedSize) {
      const uint32_t left = entry_->compressedSize - compConsumed_;
      const uint32_t want = left < kInBufSize ? left : kInBufSize;
      const int32_t n = source_->readAt(dataOffset_ + compConsumed_, inBuf_, want);
      if (n <= 0) return -1;
      inAvail_ = static_cast<uint32_t>(n);
      inPos_ = 0;
      compConsumed_ += static_cast<uint32_t>(n);
    }

    size_t inBytes = inAvail_;
    size_t outBytes = TINFL_LZ_DICT_SIZE - windowPos_;
    const mz_uint32 flags = compConsumed_ < entry_->compressedSize ? TINFL_FLAG_HAS_MORE_INPUT : 0;
    const tinfl_status status = tinfl_decompress(decomp, inBuf_ + inPos_, &inBytes, window_,
                                                 window_ + windowPos_, &outBytes, flags);
    inPos_ += static_cast<uint32_t>(inBytes);
    inAvail_ -= static_cast<uint32_t>(inBytes);
    pendingPos_ = windowPos_;
    pendingLen_ = static_cast<uint32_t>(outBytes);
    windowPos_ = (windowPos_ + static_cast<uint32_t>(outBytes)) & (TINFL_LZ_DICT_SIZE - 1);

    if (status == TINFL_STATUS_DONE) {
      done_ = true;
    } else if (status < TINFL_STATUS_DONE) {
      return -1;  // corrupt stream
    } else if (inBytes == 0 && outBytes == 0) {
      // No progress: needing input we cannot supply means truncation.
      if (status == TINFL_STATUS_NEEDS_MORE_INPUT && compConsumed_ >= entry_->compressedSize) {
        return -1;
      }
      if (status != TINFL_STATUS_NEEDS_MORE_INPUT && status != TINFL_STATUS_HAS_MORE_OUTPUT) {
        return -1;
      }
    }
  }
  produced_ += out;
  return static_cast<int32_t>(out);
}

const char* bookStatusName(BookStatus status) {
  switch (status) {
    case BookStatus::Ok: return "Ok";
    case BookStatus::IoError: return "IoError";
    case BookStatus::NotZip: return "NotZip";
    case BookStatus::NotEpub: return "NotEpub";
    case BookStatus::Encrypted: return "Encrypted";
    case BookStatus::Truncated: return "Truncated";
    case BookStatus::Unsupported: return "Unsupported";
    case BookStatus::OutOfMemory: return "OutOfMemory";
    case BookStatus::ParseError: return "ParseError";
    case BookStatus::NotFound: return "NotFound";
    case BookStatus::Stale: return "Stale";
  }
  return "Unknown";
}

}  // namespace book
}  // namespace freeink
