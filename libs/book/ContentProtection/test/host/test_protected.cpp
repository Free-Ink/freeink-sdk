// decryptedSize() must report the plaintext size from encryption.xml's
// OriginalLength, not the zip entry size (the encrypted blob). Callers size
// their output buffer from it and reject any mismatch.
#include <ProtectedBook.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using freeink::content::ProtectedBook;

struct Source : freeink::content::ByteSource {
  std::vector<uint8_t> bytes;
  uint64_t size() const override { return bytes.size(); }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (offset > bytes.size() || len > bytes.size() - offset) return -1;
    std::memcpy(dst, bytes.data() + offset, len);
    return static_cast<int32_t>(len);
  }
};

void put(std::vector<uint8_t>& b, uint32_t v, int n) {
  for (int i = 0; i < n; i++) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// Minimal zip, optionally encoding the manifest as one raw DEFLATE stored block.
Source storedZip(const std::vector<std::pair<std::string, std::string>>& files, bool deflateManifest = false) {
  Source s;
  std::vector<uint8_t> cd;
  for (const auto& f : files) {
    const bool deflated = deflateManifest && f.first == "META-INF/encryption.xml";
    std::string data;
    if (deflated) {
      assert(f.second.size() <= 65535);
      const uint16_t length = static_cast<uint16_t>(f.second.size());
      data.reserve(f.second.size() + 5);
      data.push_back(1);  // final uncompressed DEFLATE block
      data.push_back(static_cast<char>(length));
      data.push_back(static_cast<char>(length >> 8));
      data.push_back(static_cast<char>(~length));
      data.push_back(static_cast<char>(~length >> 8));
      data += f.second;
    }
    const std::string& payload = deflated ? data : f.second;
    const uint32_t offset = static_cast<uint32_t>(s.bytes.size());
    const uint32_t size = static_cast<uint32_t>(f.second.size());
    put(s.bytes, 0x04034b50, 4);
    put(s.bytes, 0, 2);  // version
    put(s.bytes, 0, 2);  // flags
    put(s.bytes, deflated ? 8 : 0, 2);
    put(s.bytes, 0, 2);  // time
    put(s.bytes, 0, 2);  // date
    put(s.bytes, 0, 4);  // crc (not checked)
    put(s.bytes, static_cast<uint32_t>(payload.size()), 4);
    put(s.bytes, size, 4);
    put(s.bytes, static_cast<uint32_t>(f.first.size()), 2);
    put(s.bytes, 0, 2);
    s.bytes.insert(s.bytes.end(), f.first.begin(), f.first.end());
    s.bytes.insert(s.bytes.end(), payload.begin(), payload.end());

    put(cd, 0x02014b50, 4);
    for (int i = 0; i < 3; i++) put(cd, 0, 2);  // versions, flags
    put(cd, deflated ? 8 : 0, 2);
    put(cd, 0, 2);  // time
    put(cd, 0, 2);  // date
    put(cd, 0, 4);
    put(cd, static_cast<uint32_t>(payload.size()), 4);
    put(cd, size, 4);
    put(cd, static_cast<uint32_t>(f.first.size()), 2);
    for (int i = 0; i < 4; i++) put(cd, 0, 2);  // extra, comment, disk, internal attrs
    put(cd, 0, 4);
    put(cd, offset, 4);
    cd.insert(cd.end(), f.first.begin(), f.first.end());
  }
  const uint32_t cdOffset = static_cast<uint32_t>(s.bytes.size());
  s.bytes.insert(s.bytes.end(), cd.begin(), cd.end());
  put(s.bytes, 0x06054b50, 4);
  put(s.bytes, 0, 2);
  put(s.bytes, 0, 2);
  put(s.bytes, static_cast<uint32_t>(files.size()), 2);
  put(s.bytes, static_cast<uint32_t>(files.size()), 2);
  put(s.bytes, static_cast<uint32_t>(cd.size()), 4);
  put(s.bytes, cdOffset, 4);
  put(s.bytes, 0, 2);
  return s;
}

// The shape a Readium LCP server emits, namespace attributes included.
std::string encEntry(const char* uri, const char* compression) {
  std::string e =
      "<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
      "<EncryptionMethod xmlns=\"http://www.w3.org/2001/04/xmlenc#\" "
      "Algorithm=\"http://www.w3.org/2001/04/xmlenc#aes256-cbc\"></EncryptionMethod>"
      "<CipherData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
      "<CipherReference xmlns=\"http://www.w3.org/2001/04/xmlenc#\" URI=\"";
  e += uri;
  e += "\"></CipherReference></CipherData>";
  if (compression) {
    e += "<EncryptionProperties><EncryptionProperty><Compression "
         "xmlns=\"http://www.idpf.org/2016/encryption#compression\" ";
    e += compression;
    e += "></Compression></EncryptionProperty></EncryptionProperties>";
  }
  return e + "</EncryptedData>";
}

int main() {
  const std::string xml =
      "<?xml version=\"1.0\"?><encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">" +
      encEntry("text/a.xhtml", "Method=\"8\" OriginalLength=\"7175\"") +
      encEntry("text/b.xhtml", "Method=\"0\" OriginalLength=\"20\"") + encEntry("text/c.xhtml", nullptr) +
      "</encryption>";
  auto src = storedZip({{"mimetype", "application/epub+zip"},
                        {"META-INF/encryption.xml", xml},
                        {"text/a.xhtml", std::string(48, 'x')},
                        {"text/b.xhtml", std::string(48, 'y')},
                        {"text/c.xhtml", std::string(32, 'z')}});

  ProtectedBook book;
  assert(book.open(src));
  assert(book.isProtected());
  assert(book.isEncrypted("text/a.xhtml") && book.isEncrypted("text/c.xhtml"));
  assert(book.decryptedSize("text/a.xhtml") == 7175);  // deflated: OriginalLength
  assert(book.decryptedSize("text/b.xhtml") == 20);    // stored: OriginalLength, not 48
  assert(book.decryptedSize("text/c.xhtml") == 32);    // no property: zip size fallback
  assert(book.decryptedSize("mimetype") == 20);
  assert(book.decryptedSize("missing") == 0);

  // XML permits whitespace around '=' in every attribute.
  std::string spacedXml = xml;
  size_t equals = 0;
  while ((equals = spacedXml.find('=', equals)) != std::string::npos) {
    spacedXml.replace(equals, 1, " \t=\n ");
    equals += 5;
  }
  auto spacedSrc = storedZip({{"META-INF/encryption.xml", spacedXml}, {"text/a.xhtml", std::string(48, 'x')}});
  ProtectedBook spacedBook;
  assert(spacedBook.open(spacedSrc));
  assert(spacedBook.isProtected() && spacedBook.isEncrypted("text/a.xhtml"));
  assert(spacedBook.decryptedSize("text/a.xhtml") == 7175);

  auto compressedSrc = storedZip({{"META-INF/encryption.xml", xml}, {"text/a.xhtml", std::string(48, 'x')}}, true);
  ProtectedBook compressedBook;
  assert(compressedBook.open(compressedSrc));
  assert(compressedBook.isProtected());
  assert(compressedBook.decryptedSize("text/a.xhtml") == 7175);

  const std::string splitXml = "<encryption><ignored padding=\"" + std::string(3000, 'x') + "\"/>" +
                               encEntry("text/a.xhtml", "Method=\"8\" OriginalLength=\"7175\"") + "</encryption>";
  for (bool deflated : {false, true}) {
    auto splitSource = storedZip({{"META-INF/encryption.xml", splitXml}}, deflated);
    ProtectedBook splitBook;
    assert(splitBook.open(splitSource));
    assert(splitBook.isEncrypted("text/a.xhtml"));
    assert(splitBook.decryptedSize("text/a.xhtml") == 7175);
  }

  const size_t manifestOffset = 30 + std::strlen("META-INF/encryption.xml");
  compressedSrc.bytes[manifestOffset] = 7;  // reserved DEFLATE block type
  for (int i = 0; i < 3; ++i) {
    ProtectedBook invalidBook;
    assert(!invalidBook.open(compressedSrc));
    assert(invalidBook.lastError() == "encryption manifest unreadable");
  }

  std::puts("test_protected: all assertions passed");
  return 0;
}
