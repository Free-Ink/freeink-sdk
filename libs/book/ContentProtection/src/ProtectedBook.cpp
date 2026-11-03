#include "ProtectedBook.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

#include "ContentMinizConfig.h"
#include "Util.h"

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

namespace freeink {
namespace content {

namespace {
constexpr const char* kEncryptionXml = "META-INF/encryption.xml";

bool tagHas(const char* tag, size_t len, const char* token) {
  const size_t tlen = strlen(token);
  if (tlen > len) return false;
  for (size_t i = 0; i + tlen <= len; i++) {
    if (memcmp(tag + i, token, tlen) == 0) return true;
  }
  return false;
}

// Extract one attribute value from a raw tag slice ('<' .. '>'). Requires
// whitespace before the name so URI= cannot match inside another attribute.
// These manifests are machine-generated with quoted attributes; both quote
// styles accepted.
bool tagAttr(const char* tag, size_t len, const char* attr, std::string* out) {
  const size_t alen = strlen(attr);
  for (size_t i = 1; i + alen + 2 < len; i++) {
    if (memcmp(tag + i, attr, alen) != 0) continue;
    const char before = tag[i - 1];
    if (before != ' ' && before != '\t' && before != '\r' && before != '\n') continue;
    size_t pos = i + alen;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (pos < len && space(tag[pos])) ++pos;
    if (pos == len || tag[pos++] != '=') continue;
    while (pos < len && space(tag[pos])) ++pos;
    if (pos == len) return false;
    const char quote = tag[pos++];
    if (quote != '"' && quote != '\'') continue;
    const char* start = tag + pos;
    const char* end = static_cast<const char*>(memchr(start, quote, len - (start - tag)));
    if (!end) return false;
    out->assign(start, static_cast<size_t>(end - start));
    return true;
  }
  return false;
}
}  // namespace

void ProtectedBook::reserveErrorBuffer() {
  if (heapProbe(96)) lastError_.reserve(64);
  lastError_.clear();
}

bool ProtectedBook::open(ByteSource& source) {
  reserveErrorBuffer();
  protected_ = false;

  if (!zip_.open(source)) {
    lastError_ = "not a zip container";
    return false;
  }

  return finishOpen(source);
}

bool ProtectedBook::openFromScan(ByteSource& source, ZipScan&& scan) {
  reserveErrorBuffer();
  protected_ = false;
  zip_ = std::move(scan);
  return finishOpen(source);
}

bool ProtectedBook::finishOpen(ByteSource& source) {
  const ZipEntryInfo* encEntry = zip_.find(kEncryptionXml);
  if (!encEntry) {
    return true;  // plain EPUB; nothing to do
  }

  // Stream-parse the manifest straight out of the zip: it enumerates every
  // encrypted resource, so its size scales with the container's file count
  // (measured ~60KB for a many-hundred-entry book) and materializing it
  // whole was OOM-aborting small-heap devices at open.
  if (!scanEncryptionXml(source, *encEntry)) {
    if (lastError_.empty()) lastError_ = "failed to read encryption.xml";
    return false;
  }

  // A manifest containing only embedded-font obfuscation needs no content
  // key or alternate read path.
  protected_ = !encryptedUriHashes_.empty();
  return true;
}

void ProtectedBook::setContentKey(const uint8_t* key, size_t len) {
  if (len != 16 && len != 32) return;
  memcpy(bookKey_, key, len);
  keyLen_ = len;
  hasKey_ = true;
}

bool ProtectedBook::isEncrypted(const std::string& name) const {
  return std::binary_search(encryptedUriHashes_.begin(), encryptedUriHashes_.end(), fnv1a64(name.data(), name.size()));
}

size_t ProtectedBook::decryptedSize(const std::string& name) const {
  // The zip records the stored blob (LCP: IV + ciphertext + padding), not the
  // plaintext; encryption.xml's OriginalLength is the real size when given.
  const uint64_t hash = fnv1a64(name.data(), name.size());
  const auto it = std::lower_bound(originalSizes_.begin(), originalSizes_.end(), std::make_pair(hash, uint32_t{0}));
  if (it != originalSizes_.end() && it->first == hash) return it->second;
  const ZipEntryInfo* entry = zip_.find(name);
  return entry ? entry->uncompressedSize : 0;
}

bool ProtectedBook::decryptEntryToSink(ByteSource& source, Crypto& crypto, const std::string& name,
                                       ContentChunkSink sink, void* context) {
  if (!hasKey_ || keyLen_ != (aes256_ ? 32u : 16u)) {
    lastError_ = "no content key";
    return false;
  }
  const ZipEntryInfo* entry = zip_.find(name);
  if (!entry) {
    // The concat allocates a temporary; fall back to the SSO-sized literal
    // (never allocates) when the heap cannot take it.
    if (heapProbe(name.size() + 48)) {
      lastError_ = "entry not found: " + name;
    } else {
      lastError_ = "entry not found";
    }
    return false;
  }
  if (entry->compressedSize < 32 || (entry->compressedSize - 16) % 16 != 0) {
    lastError_ = "entry size not AES-shaped";
    return false;
  }

  uint64_t offset = 0;
  if (!zip_.dataOffset(source, *entry, &offset)) {
    lastError_ = "entry offset unavailable";
    return false;
  }

  uint8_t iv[16];
  if (source.readAt(offset, iv, sizeof(iv)) != sizeof(iv)) {
    lastError_ = "entry IV read failed";
    return false;
  }

#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
  constexpr size_t kCipherChunk = 512;
  constexpr size_t kOutputChunk = 1024;
#else
  constexpr size_t kCipherChunk = 2048;
  constexpr size_t kOutputChunk = 4096;
#endif
  static_assert(kCipherChunk % 16 == 0, "CBC chunks must contain whole AES blocks");

  // Allocate the contiguous inflate state before the smaller streaming buffers.
  // LCP marks already-uncompressed resources Compression Method="0":
  // decrypt-only, no inflate stage.
  const bool stored =
      std::binary_search(storedUriHashes_.begin(), storedUriHashes_.end(), fnv1a64(name.data(), name.size()));

  mz_stream stream;
  memset(&stream, 0, sizeof(stream));
  if (!stored && mz_inflateInit2(&stream, -15) != MZ_OK) {
    lastError_ = "inflate setup failed";
    return false;
  }

  auto* buffers = static_cast<uint8_t*>(malloc(kCipherChunk * 2 + kOutputChunk));
  if (!buffers) {
    if (!stored) mz_inflateEnd(&stream);
    lastError_ = "insufficient memory for content stream";
    return false;
  }
  uint8_t* cipher = buffers;
  uint8_t* plain = cipher + kCipherChunk;
  uint8_t* output = plain + kCipherChunk;

  bool ended = false;
  auto inflateChunk = [&](const uint8_t* data, size_t size) {
    stream.next_in = data;
    stream.avail_in = static_cast<unsigned int>(size);
    size_t produced = 0;
    do {
      const unsigned int before = stream.avail_in;
      stream.next_out = output;
      stream.avail_out = kOutputChunk;
      const int status = mz_inflate(&stream, MZ_NO_FLUSH);
      produced = kOutputChunk - stream.avail_out;
      if (produced && (!sink || !sink(context, output, produced))) return false;
      if (status == MZ_STREAM_END) {
        ended = true;
        return true;
      }
      if (status != MZ_OK && status != MZ_BUF_ERROR) return false;
      if (status == MZ_BUF_ERROR && produced == 0 && stream.avail_in == before) {
        return stream.avail_in == 0;
      }
    } while (stream.avail_in > 0 || produced == kOutputChunk);
    return true;
  };

  auto emitChunk = [&](const uint8_t* data, size_t size) {
    if (!stored) return inflateChunk(data, size);
    if (size > 0 && (!sink || !sink(context, data, size))) return false;
    return true;
  };

  uint32_t remaining = entry->compressedSize - 16;
  offset += 16;
  bool ok = true;
  while (remaining > 0 && !ended) {
    const size_t amount = remaining < kCipherChunk ? remaining : kCipherChunk;
    if (source.readAt(offset, cipher, amount) != static_cast<int32_t>(amount)) {
      ok = false;
      lastError_ = "entry read failed";
      break;
    }
    uint8_t nextIv[16];
    memcpy(nextIv, cipher + amount - sizeof(nextIv), sizeof(nextIv));
    const bool decrypted = aes256_ ? crypto.aes256CbcDecrypt(bookKey_, iv, cipher, amount, plain)
                                   : crypto.aes128CbcDecrypt(bookKey_, iv, cipher, amount, plain);
    if (!decrypted) {
      ok = false;
      lastError_ = "content read failed";
      break;
    }
    memcpy(iv, nextIv, sizeof(iv));

    size_t plainSize = amount;
    if (amount == remaining) {
      const uint8_t pad = plain[plainSize - 1];
      if (pad >= 1 && pad <= 16 && pad <= plainSize) {
        if (aes256_) {
          // W3C xmlenc padding (LCP): the fill bytes are random; only the
          // count byte is meaningful.
          plainSize -= pad;
        } else {
          bool valid = true;
          for (size_t i = plainSize - pad; i < plainSize; i++) valid = valid && plain[i] == pad;
          if (valid) plainSize -= pad;
        }
      }
    }
    if (!emitChunk(plain, plainSize)) {
      ok = false;
      lastError_ = stored ? "content sink failed" : "inflate failed";
      break;
    }
    offset += amount;
    remaining -= amount;
  }

  if (stored) {
    ended = ok;
  } else {
    if (ok && !ended) {
      const uint8_t trailing = 'Z';
      ok = inflateChunk(&trailing, 1) && ended;
      if (!ok) lastError_ = "inflate failed";
    }
    mz_inflateEnd(&stream);
  }
  free(buffers);
  return ok && ended;
}

bool ProtectedBook::scanEncryptionXml(ByteSource& source, const ZipEntryInfo& entry) {
  encryptedUriHashes_.clear();
  storedUriHashes_.clear();
  originalSizes_.clear();
  aes256_ = false;

  uint64_t at = 0;
  if (!zip_.dataOffset(source, entry, &at)) {
    lastError_ = "entry offset unavailable";
    return false;
  }
  if (entry.method != 0 && entry.method != 8) return false;

  struct InflateGuard {
    mz_stream stream{};
    ~InflateGuard() {
      if (stream.state) mz_inflateEnd(&stream);
    }
  } inflater;
  auto& stream = inflater.stream;
  // Reserve the contiguous inflate state before smaller parser allocations.
  if (entry.method == 8) {
    const int inflateResult = mz_inflateInit2(&stream, -15);
    if (inflateResult != MZ_OK) {
      lastError_ = inflateResult == MZ_MEM_ERROR ? "manifest inflate out of memory" : "manifest inflate init failed";
      return false;
    }
  }

#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
  constexpr size_t kChunk = 512;
#else
  constexpr size_t kChunk = 2048;
#endif
  auto* bufs = static_cast<uint8_t*>(malloc(kChunk * 2));  // freed below; kept raw for the single free
  if (!bufs) {
    lastError_ = "manifest scratch out of memory";
    return false;
  }
  uint8_t* inBuf = bufs;
  uint8_t* outBuf = bufs + kChunk;

  // Tag-level scan. Inter-tag text is discarded; only element attributes
  // matter here. `carry` holds an unterminated tag across chunk boundaries —
  // manifest tags run ~200 bytes, so a tag that never closes within the cap
  // is a malformed document, not a real split.
  std::string carry;
  int pendingCipher = 0;  // bits of the EncryptionMethod just seen; 0 = unsupported
  uint64_t lastHash = 0;
  bool haveLast = false;
  bool malformed = false;
  std::string value;
  // Probe each reserve separately: these buffers need not share a contiguous block.
  {
    constexpr size_t kCarryReserve = 4096 + kChunk;
    constexpr size_t kValueReserve = 1024;  // attribute scratch (URIs)
    constexpr size_t kUriReserve = 256;     // encrypted-entry hashes (8B each)
    const auto canReserve = [this](size_t bytes, const char* error) {
      constexpr size_t kAllocationHeadroom = 64;
      if (heapProbe(bytes + kAllocationHeadroom)) return true;
      lastError_ = error;
      return false;
    };
    if (carry.capacity() < kCarryReserve && !canReserve(kCarryReserve + 1, "manifest carry out of memory")) {
      free(bufs);
      return false;
    }
    carry.reserve(kCarryReserve);
    if (value.capacity() < kValueReserve && !canReserve(kValueReserve + 1, "manifest attribute out of memory")) {
      free(bufs);
      return false;
    }
    value.reserve(kValueReserve);
    if (encryptedUriHashes_.capacity() < kUriReserve &&
        !canReserve(kUriReserve * sizeof(uint64_t), "manifest hashes out of memory")) {
      free(bufs);
      return false;
    }
    encryptedUriHashes_.reserve(kUriReserve);
    if (originalSizes_.capacity() < kUriReserve &&
        !canReserve(kUriReserve * sizeof(std::pair<uint64_t, uint32_t>), "manifest sizes out of memory")) {
      free(bufs);
      return false;
    }
    originalSizes_.reserve(kUriReserve);
    constexpr size_t kStoredReserve = 64;
    if (storedUriHashes_.capacity() < kStoredReserve &&
        !canReserve(kStoredReserve * sizeof(uint64_t), "manifest stored hashes out of memory")) {
      free(bufs);
      return false;
    }
    storedUriHashes_.reserve(kStoredReserve);
  }
  auto handleTag = [&](const char* tag, size_t len) {
    if (len < 3 || tag[1] == '/' || tag[1] == '?' || tag[1] == '!') return;
    if (tagHas(tag, len, "EncryptionMethod")) {
      pendingCipher = 0;
      if (tagAttr(tag, len, "Algorithm", &value)) {
        if (value.find("aes128-cbc") != std::string::npos) pendingCipher = 128;
        if (value.find("aes256-cbc") != std::string::npos) pendingCipher = 256;
      }
    } else if (tagHas(tag, len, "CipherReference")) {
      if (pendingCipher != 0 && tagAttr(tag, len, "URI", &value) && !value.empty()) {
        lastHash = fnv1a64(value.data(), value.size());
        haveLast = true;
        encryptedUriHashes_.push_back(lastHash);
        if (pendingCipher == 256) aes256_ = true;
      }
      pendingCipher = 0;
    } else if (tagHas(tag, len, "Compression")) {
      // Per-entry compression property (LCP): Method="0" means the plaintext
      // is not deflated. Follows the entry's CipherReference in the manifest.
      if (haveLast && tagAttr(tag, len, "Method", &value) && value == "0") {
        storedUriHashes_.push_back(lastHash);
      }
      if (haveLast && tagAttr(tag, len, "OriginalLength", &value)) {
        char* end = nullptr;
        const unsigned long n = strtoul(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == '\0' && n <= UINT32_MAX) {
          originalSizes_.emplace_back(lastHash, static_cast<uint32_t>(n));
        }
      }
    }
  };
  auto feed = [&](const uint8_t* data, size_t size) -> bool {
    carry.append(reinterpret_cast<const char*>(data), size);
    size_t pos = 0;
    for (;;) {
      const size_t lt = carry.find('<', pos);
      if (lt == std::string::npos) {
        carry.clear();
        return true;
      }
      const size_t gt = carry.find('>', lt);
      if (gt == std::string::npos) {
        carry.erase(0, lt);
        if (carry.size() > 4096) {
          malformed = true;
          return false;
        }
        return true;
      }
      handleTag(carry.data() + lt, gt - lt + 1);
      pos = gt + 1;
    }
  };

  bool ok = true;
  uint32_t remaining = entry.compressedSize;
  if (entry.method == 0) {
    while (remaining > 0 && ok) {
      const size_t amount = remaining < kChunk ? remaining : kChunk;
      ok = source.readAt(at, inBuf, amount) == static_cast<int32_t>(amount) && feed(inBuf, amount);
      at += amount;
      remaining -= static_cast<uint32_t>(amount);
    }
  } else {
    bool ended = false;
    while (remaining > 0 && ok && !ended) {
      const size_t amount = remaining < kChunk ? remaining : kChunk;
      if (source.readAt(at, inBuf, amount) != static_cast<int32_t>(amount)) {
        ok = false;
        break;
      }
      at += amount;
      remaining -= static_cast<uint32_t>(amount);
      stream.next_in = inBuf;
      stream.avail_in = static_cast<unsigned int>(amount);
      size_t produced;
      do {
        const unsigned int before = stream.avail_in;
        stream.next_out = outBuf;
        stream.avail_out = kChunk;
        const int status = mz_inflate(&stream, MZ_NO_FLUSH);
        produced = kChunk - stream.avail_out;
        if (produced && !feed(outBuf, produced)) {
          ok = false;
          break;
        }
        if (status == MZ_STREAM_END) {
          ended = true;
          break;
        }
        if (status != MZ_OK && status != MZ_BUF_ERROR) {
          ok = false;
          break;
        }
        if (status == MZ_BUF_ERROR && produced == 0 && stream.avail_in == before) break;
      } while (stream.avail_in > 0 || produced == kChunk);
    }
    ok = ok && ended;
  }
  free(bufs);

  if (!ok || malformed) {
    if (lastError_.empty()) lastError_ = "encryption manifest unreadable";
    return false;
  }
  std::sort(encryptedUriHashes_.begin(), encryptedUriHashes_.end());
  std::sort(storedUriHashes_.begin(), storedUriHashes_.end());
  std::sort(originalSizes_.begin(), originalSizes_.end());
  return true;
}

}  // namespace content
}  // namespace freeink
