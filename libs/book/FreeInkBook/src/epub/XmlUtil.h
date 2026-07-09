#pragma once

// FreeInkBook -- internal XML helpers shared by the package parsers and the
// SD-backed container catalog builder (not part of the public API).
//
// EPUB documents bind namespace prefixes inconsistently ("dc:title",
// "opf:item", or no prefix at all), so elements and attributes are matched on
// the local part of the qualified name.

#include <stddef.h>
#include <string.h>

#include "epub/XmlSax.h"

namespace freeink {
namespace book {
namespace xmlutil {

constexpr size_t kMaxTextCapture = 255;

inline const char* localName(const char* qname) {
  const char* colon = strrchr(qname, ':');
  return colon != nullptr ? colon + 1 : qname;
}

inline bool localIs(const char* qname, const char* local) {
  return strcmp(localName(qname), local) == 0;
}

inline const char* attrLocal(const char** atts, const char* local) {
  for (int i = 0; atts != nullptr && atts[i] != nullptr; i += 2) {
    if (localIs(atts[i], local)) return atts[i + 1];
  }
  return nullptr;
}

inline const char* attrExact(const char** atts, const char* qname) {
  for (int i = 0; atts != nullptr && atts[i] != nullptr; i += 2) {
    if (strcmp(atts[i], qname) == 0) return atts[i + 1];
  }
  return nullptr;
}

// True when `value` contains `token` as a whitespace-separated word (for
// attributes like properties="nav scripted").
inline bool hasToken(const char* value, const char* token) {
  if (value == nullptr) return false;
  const size_t tokenLen = strlen(token);
  const char* p = value;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    const char* start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
    if (static_cast<size_t>(p - start) == tokenLen && strncmp(start, token, tokenLen) == 0) {
      return true;
    }
  }
  return false;
}

// Fixed-capacity accumulator for element text (titles, labels). Content
// beyond the cap is truncated, never overflowed.
struct TextCapture {
  char buf[kMaxTextCapture + 1];
  size_t len = 0;
  bool active = false;

  void begin() {
    active = true;
    len = 0;
    buf[0] = '\0';
  }
  void end() { active = false; }
  void add(const char* text, int textLen) {
    if (!active || textLen <= 0) return;
    size_t n = static_cast<size_t>(textLen);
    if (len + n > kMaxTextCapture) n = kMaxTextCapture - len;
    memcpy(buf + len, text, n);
    len += n;
    buf[len] = '\0';
  }
  // Trims leading/trailing ASCII whitespace in place and returns the string.
  const char* trimmed() {
    size_t start = 0;
    while (start < len && (buf[start] == ' ' || buf[start] == '\t' || buf[start] == '\n' ||
                           buf[start] == '\r')) {
      ++start;
    }
    size_t stop = len;
    while (stop > start && (buf[stop - 1] == ' ' || buf[stop - 1] == '\t' ||
                            buf[stop - 1] == '\n' || buf[stop - 1] == '\r')) {
      --stop;
    }
    buf[stop] = '\0';
    return buf + start;
  }
};

// META-INF/encryption.xml does NOT always mean DRM: retail EPUBs routinely
// declare only obfuscated embedded fonts (IDPF or Adobe mangling), which the
// engine never reads anyway. Only actual content encryption (ADEPT, LCP --
// anything beyond the two font-obfuscation algorithms) makes a book
// unreadable.
class EncryptionScan : public XmlHandler {
 public:
  void onStartElement(const char* name, const char** atts) override {
    if (!localIs(name, "EncryptionMethod")) return;
    for (int i = 0; atts != nullptr && atts[i] != nullptr; i += 2) {
      if (!localIs(atts[i], "Algorithm")) continue;
      const char* alg = atts[i + 1];
      const bool fontObfuscation = strcmp(alg, "http://www.idpf.org/2008/embedding") == 0 ||
                                   strcmp(alg, "http://ns.adobe.com/pdf/enc#RC") == 0;
      if (!fontObfuscation) contentEncrypted = true;
    }
  }
  bool contentEncrypted = false;
};

}  // namespace xmlutil
}  // namespace book
}  // namespace freeink
