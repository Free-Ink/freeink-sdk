#pragma once

// FreeInk — encrypted-entry read path.
//
// Access-only, by design:
//  - content stays encrypted at rest (items are accessed on read, into memory)
//  - the content key is unwrapped with this device's access credential
//  - access expiry from rights.xml is enforced by the caller (isExpired)
// There is deliberately no API that writes the content out in the clear.
//
// Freestanding C++17. Crypto and storage are injected.

#include <stdint.h>

#include <string>
#include <vector>

#include "ByteSource.h"
#include "ContentProtection.h"
#include "Crypto.h"
#include "Credential.h"
#include "Rights.h"
#include "Zip.h"

namespace freeink {
namespace content {

class ProtectedBook {
 public:
  // Opens a (possibly protected) EPUB: scans the container, and when
  // META-INF/encryption.xml is present, parses rights/encryption metadata and
  // unwraps the content key with the credential's private key.
  //
  // rightsXmlOverride: the access-grant rights document (wrapped content key),
  // supplied out-of-band. the source delivers rights.xml separately from the EPUB, so
  // the preferred flow keeps it in a sidecar and passes it here — the EPUB on
  // disk stays byte-identical to what was delivered. When empty, falls back to
  // reading META-INF/rights.xml from inside the zip (legacy in-container form).
  bool open(ByteSource& source, Crypto& crypto, const Credential& identity,
            const std::string& rightsXmlOverride = "");

  // Completes open using an already-scanned ZIP index. This lets an embedding
  // application classify a plain EPUB before initializing crypto or loading
  // credentials, then transfer ownership of that same index instead of
  // scanning the central directory a second time.
  bool openFromScan(ByteSource& source, Crypto& crypto, const Credential& identity,
                    ZipScan&& scan, const std::string& rightsXmlOverride = "");

  bool isProtected() const { return protected_; }
  const Rights& rights() const { return rights_; }
  const std::string& lastError() const { return lastError_; }

  // Loan expiry (0 = none found). Caller enforces: refuse to open past due.
  int64_t expiresAt() const { return rights_.expiresAt; }
  bool isExpired(int64_t nowEpoch) const { return rights_.expiresAt != 0 && nowEpoch > rights_.expiresAt; }

  bool isEncrypted(const std::string& name) const;
  size_t decryptedSize(const std::string& name) const;

  // Access + inflate one protected entry, streamed through the sink. The only
  // read path: callers own their buffering, so no whole-entry allocation can
  // hide in here.
  bool decryptEntryToSink(ByteSource& source, Crypto& crypto, const std::string& name,
                          ContentChunkSink sink, void* context);

  // Read a non-encrypted entry fully, inflating when deflated. Capped and
  // OOM-safe: fails (rather than aborting) when the entry is oversized or
  // memory is unavailable.
  bool readEntryInflated(ByteSource& source, const std::string& name, std::string* out);

  // Inflates raw-deflate data (windowBits -15) into a caller-owned buffer of
  // exactly the expected size.
  bool inflateTo(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen);

 private:
  bool finishOpen(ByteSource& source, Crypto& crypto, const Credential& identity,
                  const std::string& rightsXmlOverride);
  bool unwrapBookKey(Crypto& crypto, const Credential& identity, uint8_t out[16]);
  // Stream-parses encryption.xml out of the zip in chunks, keeping only path
  // hashes. The manifest scales with the container's file count, so it is
  // never materialized whole.
  bool scanEncryptionXml(ByteSource& source, const ZipEntryInfo& entry);

  ZipScan zip_;
  Rights rights_;
  // Sorted FNV-1a hashes of the aes128-cbc encrypted entry paths.
  std::vector<uint64_t> encryptedUriHashes_;
  uint8_t bookKey_[16] = {0};
  bool protected_ = false;
  std::string lastError_;
};

}  // namespace content
}  // namespace freeink
