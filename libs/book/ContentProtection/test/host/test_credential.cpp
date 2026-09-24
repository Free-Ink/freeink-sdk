// Host tests for credential parsing. No device or PlatformIO needed --
// Credential.cpp is freestanding C++17 and depends only on ByteSource.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "Credential.h"

using freeink::content::ByteSource;
using freeink::content::Credential;
using freeink::content::parseCredential;

namespace {

class MemorySource : public ByteSource {
 public:
  explicit MemorySource(std::string data) : data_(std::move(data)) {}
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (offset >= data_.size()) return 0;
    const size_t avail = data_.size() - static_cast<size_t>(offset);
    const size_t n = len < avail ? len : avail;
    memcpy(dst, data_.data() + offset, n);
    return static_cast<int32_t>(n);
  }
  uint64_t size() const override { return sizeOverride_ ? sizeOverride_ : data_.size(); }
  void setSizeOverride(uint64_t v) { sizeOverride_ = v; }

 private:
  std::string data_;
  uint64_t sizeOverride_ = 0;
};

const char* kValid =
    "FREEINK-CONTENT-KEY 1\n"
    "username: reader\n"
    "userUuid: u-123\n"
    "deviceUuid: d-456\n"
    "privateLicenseKey: BASE64KEY==\n";

void testValid() {
  Credential c;
  assert(parseCredential(std::string(kValid), &c));
  assert(c.username == "reader");
  assert(c.userUuid == "u-123");
  assert(c.deviceUuid == "d-456");
  assert(c.privateLicenseKey == "BASE64KEY==");
  assert(c.complete());
}

void testCrlfAndBlankLines() {
  Credential c;
  const std::string text =
      "FREEINK-CONTENT-KEY 1\r\n"
      "\r\n"
      "userUuid: u-1\r\n"
      "deviceUuid: d-1\r\n"
      "privateLicenseKey: K\r\n";
  assert(parseCredential(text, &c));
  assert(c.userUuid == "u-1");
  assert(c.privateLicenseKey == "K");
}

void testNoTrailingNewline() {
  Credential c;
  std::string text(kValid);
  text.pop_back();  // drop the final '\n'
  assert(parseCredential(text, &c));
  assert(c.privateLicenseKey == "BASE64KEY==");
}

void testRejects() {
  Credential c1;
  assert(!parseCredential(std::string("NOT-A-HEADER\nuserUuid: u\n"), &c1));

  Credential c2;  // header present, mandatory fields missing
  assert(!parseCredential(std::string("FREEINK-CONTENT-KEY 1\nusername: r\n"), &c2));

  Credential c3;
  assert(!parseCredential(std::string(""), &c3));
}

void testUnknownFieldsIgnored() {
  Credential c;
  const std::string text =
      "FREEINK-CONTENT-KEY 1\n"
      "signingCertPem: whatever\n"
      "somethingNew: value\n"
      "no-colon-here\n"
      "userUuid: u\n"
      "deviceUuid: d\n"
      "privateLicenseKey: K\n";
  assert(parseCredential(text, &c));
  assert(c.complete());
}

// A value containing ": " must not be truncated at the second separator.
void testValueWithSeparator() {
  Credential c;
  const std::string text =
      "FREEINK-CONTENT-KEY 1\n"
      "userUuid: u\n"
      "deviceUuid: d\n"
      "privateLicenseKey: a: b: c\n";
  assert(parseCredential(text, &c));
  assert(c.privateLicenseKey == "a: b: c");
}

void testByteSource() {
  MemorySource src{std::string(kValid)};
  Credential c;
  assert(parseCredential(src, &c));
  assert(c.userUuid == "u-123");
}

// A source reporting a size beyond the bundle cap is refused outright, and one
// reporting zero never reaches the allocation.
void testByteSourceSizeGuards() {
  MemorySource tooBig{std::string(kValid)};
  tooBig.setSizeOverride(256 * 1024 + 1);
  Credential c1;
  assert(!parseCredential(tooBig, &c1));

  MemorySource empty{std::string()};
  Credential c2;
  assert(!parseCredential(empty, &c2));
}

// size() may over-report (short read at EOF); only the bytes actually read are
// parsed, and the tail of the buffer is never treated as content.
void testShortRead() {
  MemorySource src{std::string(kValid)};
  src.setSizeOverride(std::strlen(kValid) + 4096);
  Credential c;
  assert(parseCredential(src, &c));
  assert(c.privateLicenseKey == "BASE64KEY==");
}

// A ByteSource that over-reports how much it read must not make the parser
// walk past the end of its buffer.
class LyingSource : public ByteSource {
 public:
  int32_t readAt(uint64_t, void* dst, uint32_t len) override {
    memset(dst, 'A', len);
    return static_cast<int32_t>(len) + 4096;  // claims more than it wrote
  }
  uint64_t size() const override { return 64; }
};

void testOverReportedRead() {
  LyingSource src;
  Credential c;
  assert(!parseCredential(src, &c));  // garbage, but must not read out of bounds
}

}  // namespace

int main() {
  testValid();
  testCrlfAndBlankLines();
  testNoTrailingNewline();
  testRejects();
  testUnknownFieldsIgnored();
  testValueWithSeparator();
  testByteSource();
  testByteSourceSizeGuards();
  testShortRead();
  testOverReportedRead();
  printf("credential: all tests passed\n");
  return 0;
}
