#pragma once

// FreeInk — access credential.
//
// The credential bundle is produced off-device and stored on the SD card so
// it is portable across firmware installs —
// CrossPoint users switch firmware builds and the credential must survive.
// FAT32 has no permissions; the private key inside is sensitive — treat the
// file accordingly.
//
// Line-oriented `key: value` format (FREEINK-CONTENT-KEY 1) so parsing
// needs no JSON library. Unknown lines are ignored (forward-compatible).
//
// Freestanding C++17.

#include <stdint.h>

#include <string>

#include "ByteSource.h"

namespace freeink {
namespace content {

struct Credential {
  std::string username;
  std::string userUuid;
  std::string deviceUuid;
  std::string serial;
  std::string fingerprint;
  std::string privateLicenseKey;  // base64 PKCS#8 DER (clear; sensitive)
  std::string deviceSalt;         // base64 (optional for read-only use)
  bool complete() const {
    return !userUuid.empty() && !deviceUuid.empty() && !privateLicenseKey.empty();
  }
};

// Parses an identity bundle. Returns false when the file can't be read or
// the mandatory fields are missing.
bool parseCredential(ByteSource& source, Credential* out);
bool parseCredential(const std::string& text, Credential* out);

}  // namespace content
}  // namespace freeink
