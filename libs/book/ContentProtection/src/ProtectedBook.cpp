#include "ProtectedBook.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

#include "ContentMinizConfig.h"
#include "Util.h"

namespace freeink {
namespace content {

namespace {
constexpr const char* kEncryptionXml = "META-INF/encryption.xml";
constexpr const char* kRightsXml = "META-INF/rights.xml";
constexpr size_t kRsaBlock = 128;  // RSA-1024

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
    if (memcmp(tag + i, attr, alen) != 0 || tag[i + alen] != '=') continue;
    const char before = tag[i - 1];
    if (before != ' ' && before != '\t' && before != '\r' && before != '\n') continue;
    const char quote = tag[i + alen + 1];
    if (quote != '"' && quote != '\'') continue;
    const char* start = tag + i + alen + 2;
    const char* end = static_cast<const char*>(memchr(start, quote, len - (start - tag)));
    if (!end) return false;
    out->assign(start, static_cast<size_t>(end - start));
    return true;
  }
  return false;
}
}  // namespace

bool ProtectedBook::open(ByteSource& source, Crypto& crypto, const Credential& identity,
                         const std::string& rightsXmlOverride) {
  lastError_.clear();
  protected_ = false;

  if (!zip_.open(source)) {
    lastError_ = "not a zip container";
    return false;
  }

  return finishOpen(source, crypto, identity, rightsXmlOverride);
}

bool ProtectedBook::openFromScan(ByteSource& source, Crypto& crypto,
                                 const Credential& identity, ZipScan&& scan,
                                 const std::string& rightsXmlOverride) {
  lastError_.clear();
  protected_ = false;
  zip_ = std::move(scan);
  return finishOpen(source, crypto, identity, rightsXmlOverride);
}

bool ProtectedBook::finishOpen(ByteSource& source, Crypto& crypto,
                               const Credential& identity,
                               const std::string& rightsXmlOverride) {
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

  // A manifest containing only embedded-font obfuscation needs no key unwrap
  // or alternate read path.
  if (encryptedUriHashes_.empty()) {
    return true;  // protected_ stays false
  }

  // Prefer the out-of-band rights document (sidecar); fall back to reading
  // META-INF/rights.xml from the zip for the legacy in-container form.
  std::string rightsXml = rightsXmlOverride;
  if (rightsXml.empty() && !readEntryInflated(source, kRightsXml, &rightsXml)) {
    lastError_ = "failed to read rights.xml";
    return false;
  }
  if (!parseRightsXml(rightsXml, &rights_)) {
    lastError_ = "failed to parse rights.xml";
    return false;
  }

  if (!rights_.user.empty() && !identity.userUuid.empty() && rights_.user != identity.userUuid) {
    lastError_ = "registered to a different account";
    return false;
  }

  if (!unwrapBookKey(crypto, identity, bookKey_)) {
    lastError_ = "content key unavailable (wrong/incomplete credential?)";
    return false;
  }

  protected_ = true;
  return true;
}

bool ProtectedBook::isEncrypted(const std::string& name) const {
  return std::binary_search(encryptedUriHashes_.begin(), encryptedUriHashes_.end(),
                            fnv1a64(name.data(), name.size()));
}

size_t ProtectedBook::decryptedSize(const std::string& name) const {
  const ZipEntryInfo* entry = zip_.find(name);
  return entry ? entry->uncompressedSize : 0;
}

bool ProtectedBook::unwrapBookKey(Crypto& crypto, const Credential& identity, uint8_t out[16]) {
  std::string encryptedKey = base64Decode(rights_.encryptedKey);

  // Optional first pass (newer ACS variants): the wrapped key is itself
  // AES-encrypted with a key derived from the keyType attribute and an IV
  // derived from the device/fulfillment/voucher UUIDs.
  if (!rights_.keyType.empty()) {
    uint8_t digest[32];
    crypto.sha256(reinterpret_cast<const uint8_t*>(rights_.keyType.data()), rights_.keyType.size(),
                  digest);
    const long nonce = strtol(rights_.keyType.c_str(), nullptr, 10);
    // keyType is attacker-controlled (from rights.xml); C++ % keeps the sign, so
    // a negative value would make `remainder` negative and drive the memcpys out
    // of bounds. Normalize into [0,16) — matches the reference's unsigned rotate.
    const int remainder = static_cast<int>(((nonce % 16) + 16) % 16);

    uint8_t key[16];
    memcpy(key, digest + remainder * 2, 16 - remainder);
    memcpy(key + (16 - remainder), digest + remainder, remainder);

    uint8_t deviceId[16], fulfillmentId[16], voucherId[16], iv[16];
    if (!uuidBytes(rights_.device, deviceId) || !uuidBytes(rights_.fulfillment, fulfillmentId) ||
        !uuidBytes(rights_.voucher, voucherId)) {
      return false;
    }
    for (int i = 0; i < 16; i++) iv[i] = deviceId[i] ^ fulfillmentId[i] ^ voucherId[i];

    std::vector<uint8_t> firstPass(encryptedKey.size());
    if (!crypto.aes128CbcDecrypt(key, iv, reinterpret_cast<const uint8_t*>(encryptedKey.data()),
                                 encryptedKey.size(), firstPass.data())) {
      return false;
    }
    // A trailing 16x 0x10 block is OpenSSL padding; remove when present.
    size_t len = firstPass.size();
    bool padded = len >= 16;
    for (size_t i = len - 16; padded && i < len; i++) padded = firstPass[i] == 0x10;
    if (padded) len -= 16;
    encryptedKey.assign(reinterpret_cast<const char*>(firstPass.data()), len);
  }

  if (encryptedKey.size() != kRsaBlock) return false;

  const std::string pkcs8 = base64Decode(identity.privateLicenseKey);
  if (pkcs8.empty()) return false;

  uint8_t block[kRsaBlock];
  const int32_t n = crypto.rsaPrivateRaw(
      reinterpret_cast<const uint8_t*>(pkcs8.data()), pkcs8.size(),
      reinterpret_cast<const uint8_t*>(encryptedKey.data()), encryptedKey.size(), block,
      sizeof(block));
  if (n != static_cast<int32_t>(sizeof(block))) return false;

  // PKCS#1 v1.5 type-2 padding: 0x00 0x02 <random nonzero> 0x00 <key at end>.
  if (block[0] != 0x00 || block[1] != 0x02 || block[sizeof(block) - 16 - 1] != 0x00) return false;
  memcpy(out, block + sizeof(block) - 16, 16);
  return true;
}

bool ProtectedBook::decryptEntryToSink(ByteSource& source, Crypto& crypto,
                                       const std::string& name, ContentChunkSink sink,
                                       void* context) {
  const ZipEntryInfo* entry = zip_.find(name);
  if (!entry) {
    lastError_ = "entry not found: " + name;
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

  constexpr size_t kCipherChunk = 2048;
  constexpr size_t kOutputChunk = 4096;
  auto* buffers = static_cast<uint8_t*>(malloc(kCipherChunk * 2 + kOutputChunk));
  if (!buffers) {
    lastError_ = "insufficient memory for content stream";
    return false;
  }
  uint8_t* cipher = buffers;
  uint8_t* plain = cipher + kCipherChunk;
  uint8_t* output = plain + kCipherChunk;

  mz_stream stream;
  memset(&stream, 0, sizeof(stream));
  if (mz_inflateInit2(&stream, -15) != MZ_OK) {
    free(buffers);
    lastError_ = "inflate setup failed";
    return false;
  }

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
    if (!crypto.aes128CbcDecrypt(bookKey_, iv, cipher, amount, plain)) {
      ok = false;
      lastError_ = "content read failed";
      break;
    }
    memcpy(iv, nextIv, sizeof(iv));

    size_t plainSize = amount;
    if (amount == remaining) {
      const uint8_t pad = plain[plainSize - 1];
      if (pad >= 1 && pad <= 16 && pad <= plainSize) {
        bool valid = true;
        for (size_t i = plainSize - pad; i < plainSize; i++) valid = valid && plain[i] == pad;
        if (valid) plainSize -= pad;
      }
    }
    if (!inflateChunk(plain, plainSize)) {
      ok = false;
      lastError_ = "inflate failed";
      break;
    }
    offset += amount;
    remaining -= amount;
  }

  if (ok && !ended) {
    const uint8_t trailing = 'Z';
    ok = inflateChunk(&trailing, 1) && ended;
    if (!ok) lastError_ = "inflate failed";
  }

  mz_inflateEnd(&stream);
  free(buffers);
  return ok && ended;
}

bool ProtectedBook::scanEncryptionXml(ByteSource& source, const ZipEntryInfo& entry) {
  encryptedUriHashes_.clear();

  uint64_t at = 0;
  if (!zip_.dataOffset(source, entry, &at)) {
    lastError_ = "entry offset unavailable";
    return false;
  }
  if (entry.method != 0 && entry.method != 8) return false;

  constexpr size_t kChunk = 2048;
  auto* bufs = static_cast<uint8_t*>(malloc(kChunk * 2));  // freed below; kept raw for the single free
  if (!bufs) {
    lastError_ = "out of memory";
    return false;
  }
  uint8_t* inBuf = bufs;
  uint8_t* outBuf = bufs + kChunk;

  // Tag-level scan. Inter-tag text is discarded; only element attributes
  // matter here. `carry` holds an unterminated tag across chunk boundaries —
  // manifest tags run ~200 bytes, so a tag that never closes within the cap
  // is a malformed document, not a real split.
  std::string carry;
  bool aes128 = false;
  bool malformed = false;
  std::string value;
  auto handleTag = [&](const char* tag, size_t len) {
    if (len < 3 || tag[1] == '/' || tag[1] == '?' || tag[1] == '!') return;
    if (tagHas(tag, len, "EncryptionMethod")) {
      aes128 = tagAttr(tag, len, "Algorithm", &value) && value.find("aes128-cbc") != std::string::npos;
    } else if (tagHas(tag, len, "CipherReference")) {
      if (aes128 && tagAttr(tag, len, "URI", &value) && !value.empty()) {
        encryptedUriHashes_.push_back(fnv1a64(value.data(), value.size()));
      }
      aes128 = false;
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
    mz_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (mz_inflateInit2(&stream, -15) != MZ_OK) {
      free(bufs);
      return false;
    }
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
    mz_inflateEnd(&stream);
    ok = ok && ended;
  }
  free(bufs);

  if (!ok || malformed) {
    if (lastError_.empty()) lastError_ = "encryption manifest unreadable";
    return false;
  }
  std::sort(encryptedUriHashes_.begin(), encryptedUriHashes_.end());
  return true;
}

bool ProtectedBook::readEntryInflated(ByteSource& source, const std::string& name, std::string* out) {
  const ZipEntryInfo* entry = zip_.find(name);
  if (!entry) return false;

  // Only metadata documents (encryption.xml / rights.xml) are read this way.
  // Cap them: a huge (or hostile) entry must not sink a small-heap device.
  // encryption.xml for a many-hundred-entry book measures ~60KB.
  constexpr size_t kMaxEntrySize = 128 * 1024;
  const size_t outSize = entry->method == 0 ? entry->compressedSize : entry->uncompressedSize;
  if (entry->compressedSize > kMaxEntrySize || outSize > kMaxEntrySize) {
    lastError_ = "metadata entry too large";
    return false;
  }

  std::unique_ptr<uint8_t[]> raw(new (std::nothrow) uint8_t[entry->compressedSize]);
  if (!raw) {
    lastError_ = "out of memory";
    return false;
  }
  if (!zip_.readRaw(source, *entry, raw.get())) return false;

  // Inflate straight into the output string: one output buffer total, where
  // the previous shape peaked at compressed + 2x uncompressed. String growth
  // cannot report failure (it aborts with exceptions disabled), so prove the
  // allocation is possible with a nothrow probe first.
  {
    void* probe = malloc(outSize + 32);
    if (!probe) {
      lastError_ = "out of memory";
      return false;
    }
    free(probe);
  }
  out->resize(outSize);

  if (entry->method == 0) {
    memcpy(out->data(), raw.get(), outSize);
    return true;
  }
  if (entry->method != 8) return false;
  return inflateTo(raw.get(), entry->compressedSize, reinterpret_cast<uint8_t*>(out->data()), outSize);
}

bool ProtectedBook::inflateTo(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
  mz_stream stream;
  memset(&stream, 0, sizeof(stream));
  stream.next_in = in;
  stream.avail_in = static_cast<unsigned int>(inLen);
  stream.next_out = out;
  stream.avail_out = static_cast<unsigned int>(outLen);

  if (mz_inflateInit2(&stream, -15) != MZ_OK) return false;
  // The whole input and output are in memory, so a single MZ_FINISH pass
  // suffices; anything but a clean end at exactly outLen is a corrupt entry.
  const int status = mz_inflate(&stream, MZ_FINISH);
  const bool ok = status == MZ_STREAM_END && stream.avail_out == 0;
  mz_inflateEnd(&stream);
  return ok;
}

}  // namespace content
}  // namespace freeink
