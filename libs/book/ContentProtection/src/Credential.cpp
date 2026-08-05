#include "Credential.h"

namespace freeink {
namespace content {

namespace {

void assignField(const std::string& key, const std::string& value, Credential* out) {
  if (key == "username") out->username = value;
  else if (key == "userUuid") out->userUuid = value;
  else if (key == "deviceUuid") out->deviceUuid = value;
  else if (key == "serial") out->serial = value;
  else if (key == "fingerprint") out->fingerprint = value;
  else if (key == "privateLicenseKey") out->privateLicenseKey = value;
  else if (key == "devicesalt") out->deviceSalt = value;
  // signingKeyPem / signingCertPem / future fields: accepted, ignored here
}

}  // namespace

bool parseCredential(const std::string& text, Credential* out) {
  size_t pos = 0;
  bool headerSeen = false;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    const size_t end = (nl == std::string::npos) ? text.size() : nl;
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    if (!headerSeen) {
      if (line.rfind("FREEINK-CONTENT-KEY", 0) != 0) return false;
      headerSeen = true;
      continue;
    }

    const size_t colon = line.find(": ");
    if (colon == std::string::npos) continue;
    assignField(line.substr(0, colon), line.substr(colon + 2), out);
  }
  return headerSeen && out->complete();
}

bool parseCredential(ByteSource& source, Credential* out) {
  const uint64_t size = source.size();
  if (size == 0 || size > 256 * 1024) return false;  // bundles are a few KB
  std::string text;
  text.resize(static_cast<size_t>(size));
  const int32_t n = source.readAt(0, text.data(), static_cast<uint32_t>(size));
  if (n <= 0) return false;
  text.resize(static_cast<size_t>(n));
  return parseCredential(text, out);
}

}  // namespace content
}  // namespace freeink
