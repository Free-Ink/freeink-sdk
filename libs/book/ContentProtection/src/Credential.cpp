#include "Credential.h"

#include <memory>
#include <new>
#include <string_view>

namespace freeink {
namespace content {

namespace {

// Bundles are a few KB; the cap only guards against a wildly wrong size().
constexpr uint64_t kMaxBundleBytes = 256 * 1024;

void assignField(std::string_view key, std::string_view value, Credential* out) {
  std::string* field = nullptr;
  if (key == "username") field = &out->username;
  else if (key == "userUuid") field = &out->userUuid;
  else if (key == "deviceUuid") field = &out->deviceUuid;
  else if (key == "serial") field = &out->serial;
  else if (key == "fingerprint") field = &out->fingerprint;
  else if (key == "privateLicenseKey") field = &out->privateLicenseKey;
  else if (key == "devicesalt") field = &out->deviceSalt;
  // signingKeyPem / signingCertPem / future fields: accepted, ignored here
  if (field) field->assign(value.begin(), value.end());
}

// Slices the text in place. The only allocations left are the credential's own
// fields -- see the note on parseCredential(ByteSource&) for why that matters.
bool parseText(std::string_view text, Credential* out) {
  bool headerSeen = false;
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = (nl == std::string_view::npos) ? std::string_view() : text.substr(nl + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;

    if (!headerSeen) {
      if (line.rfind("FREEINK-CONTENT-KEY", 0) != 0) return false;
      headerSeen = true;
      continue;
    }

    const size_t colon = line.find(": ");
    if (colon == std::string_view::npos) continue;
    assignField(line.substr(0, colon), line.substr(colon + 2), out);
  }
  return headerSeen && out->complete();
}

}  // namespace

bool parseCredential(const std::string& text, Credential* out) {
  return parseText(text, out);
}

bool parseCredential(ByteSource& source, Credential* out) {
  const uint64_t size = source.size();
  if (size == 0 || size > kMaxBundleBytes) return false;

  // Nothrow, because this runs on targets built with -fno-exceptions, where a
  // failed operator new calls abort() rather than returning. Reading the
  // bundle into a std::string took a throwing allocation of up to the cap above,
  // which on a small-RAM target is taken against whatever heap happens to be left.
  const size_t capacity = static_cast<size_t>(size);
  auto buf = std::unique_ptr<char[]>(new (std::nothrow) char[capacity]);
  if (!buf) return false;

  const int32_t n = source.readAt(0, buf.get(), static_cast<uint32_t>(capacity));
  if (n <= 0) return false;
  // ByteSource is implemented by the caller, so treat a length longer than the
  // buffer as the source over-reporting rather than reading past the end.
  size_t got = static_cast<size_t>(n);
  if (got > capacity) got = capacity;
  return parseText(std::string_view(buf.get(), got), out);
}

}  // namespace content
}  // namespace freeink
