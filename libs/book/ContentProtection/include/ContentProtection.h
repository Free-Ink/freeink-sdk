#pragma once

// Reader-facing interface for encrypted entries.
//
// Freestanding C++17 core; storage/crypto injected. Nothing here can write the
// decoded bytes to storage; access is on-read, into memory only.

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace freeink {
namespace content {

using ContentChunkSink = bool (*)(void* context, const uint8_t* data, size_t size);

// Provides streamed access to protected items of an opened container.
class ContentDecryptor {
 public:
  virtual ~ContentDecryptor() = default;
  // Is this container-relative item stored encrypted?
  virtual bool isEncrypted(const std::string& itemPath) const = 0;
  virtual size_t decryptedSize(const std::string& itemPath) const = 0;
  // Read one protected item as chunks through the sink; the caller owns all
  // buffering (no whole-entry allocation happens on this side).
  virtual bool decryptToSink(const std::string& itemPath, ContentChunkSink sink,
                             void* context) = 0;
};

// Opens the protected read path for `epubPath` when the content is protected
// and this device holds the credential to read it. Returns:
//   non-null            -> route protected item reads through it
//   null, err empty     -> plain content (not protected) — read normally
//   null, err non-empty -> protected but unreadable (no credential / expired)
//
// This is the SD-backed convenience entry; the integrating firmware provides
// the implementation (it binds the ByteSource to device storage). The portable
// lib itself stays storage-agnostic — host code opens ProtectedBook directly.
std::unique_ptr<ContentDecryptor> openProtectedBook(const std::string& epubPath, std::string& err);

}  // namespace content
}  // namespace freeink
