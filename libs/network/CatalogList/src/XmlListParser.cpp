#include "XmlListParser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {
void splitSelector(const std::string& s, bool& onItemTag, std::string& elem, std::string& attr) {
  if (s.empty()) return;
  if (s[0] == '@') {
    onItemTag = true;
    attr = s.substr(1);
    return;
  }
  const size_t at = s.find('@');
  elem = at == std::string::npos ? s : s.substr(0, at);
  if (at != std::string::npos) attr = s.substr(at + 1);
}

const char* localName(const XML_Char* name) {
  const char* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

const char* findAttr(const XML_Char** atts, const std::string& attr) {
  for (int i = 0; atts[i]; i += 2) {
    if (attr == atts[i]) return atts[i + 1];
  }
  return nullptr;
}

// Percent-decodes a URL path. '+' stays literal: it means a space only in form
// encoding, never in a path ("/books/A+B.epub" is a file named "A+B.epub").
std::string urlDecode(const std::string& s) {
  const auto hex = [](char c) { return isxdigit(static_cast<unsigned char>(c)); };
  const auto val = [](char c) { return isdigit(static_cast<unsigned char>(c)) ? c - '0' : (tolower(c) - 'a' + 10); };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) && hex(s[i + 2])) {
      out += static_cast<char>(val(s[i + 1]) << 4 | val(s[i + 2]));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string urlEncodePath(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 2);
  for (const unsigned char c : s) {
    if (isalnum(c) || strchr("-_.~/", c)) {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// Splits scheme://host[:port] from the path ("/" when there is none).
void splitUrl(const std::string& url, std::string& origin, std::string& path) {
  const size_t schemeEnd = url.find("://");
  const size_t hostEnd = url.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
  origin = hostEnd == std::string::npos ? url : url.substr(0, hostEnd);
  path = hostEnd == std::string::npos ? "/" : url.substr(hostEnd);
}

std::string trimSlashes(std::string s) {
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

bool endsWithNoCase(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size()) return false;
  return std::equal(suffix.begin(), suffix.end(), s.end() - suffix.size(), [](char a, char b) {
    return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b));
  });
}

void trim(std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) {
    s.clear();
    return;
  }
  const size_t e = s.find_last_not_of(" \t\r\n");
  s = s.substr(b, e - b + 1);
}
}  // namespace

XmlListParser::XmlListParser(const std::string& itemName, const std::string& containerName,
                             const std::string* const (&selectors)[F_COUNT], const ItemSink sink, void* sinkCtx)
    : item(itemName), container(containerName), sink(sink), sinkCtx(sinkCtx) {
  for (int i = 0; i < F_COUNT; i++) splitSelector(*selectors[i], sel[i].onItemTag, sel[i].elem, sel[i].attr);
}

void XmlListParser::setUrlOptions(UrlOptions options) {
  urls = std::move(options);
  std::string path;
  splitUrl(urls.requestUrl, origin, path);
  const std::string decodedPath = urlDecode(path);
  decodedSelf = trimSlashes(decodedPath);
  requestDir = decodedPath.substr(0, decodedPath.rfind('/') + 1);  // "/books/x" and "/books/" -> "/books/"
}

bool XmlListParser::acceptRow(RawItem& row) const {
  std::string& url = row.field[F_URL];
  if (url.empty()) return false;
  // Every href reduces to a decoded absolute path: an absolute URL keeps its
  // path, "/x" is already one, and a relative "x" joins the listing's folder.
  const bool absoluteUrl = url.find("://") != std::string::npos;
  std::string decoded;
  if (absoluteUrl) {
    std::string unusedOrigin;
    splitUrl(url, unusedOrigin, decoded);
    decoded = urlDecode(decoded);
  } else {
    decoded = urlDecode(url);
    if (decoded.front() != '/') decoded = requestDir + decoded;
  }
  const std::string trimmed = trimSlashes(decoded);
  if (urls.skipSelf && trimmed == decodedSelf) return false;
  if (!row.isDir && !urls.extensions.empty() &&
      std::none_of(urls.extensions.begin(), urls.extensions.end(),
                   [&](const std::string& ext) { return endsWithNoCase(trimmed, ext); })) {
    return false;
  }
  if (row.field[F_TITLE].empty()) {
    const size_t slash = trimmed.rfind('/');
    row.field[F_TITLE] = slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
  }
  if (urls.resolveUrls && !absoluteUrl) url = origin + urlEncodePath(decoded);
  return true;
}

bool XmlListParser::parse(const ReadFn read, void* readCtx) {
  XML_Parser p = XML_ParserCreate(nullptr);
  if (!p) return false;
  XML_SetUserData(p, this);
  XML_SetElementHandler(
      p,
      [](void* self, const XML_Char* name, const XML_Char** atts) {
        static_cast<XmlListParser*>(self)->onStart(name, atts);
      },
      [](void* self, const XML_Char* name) { static_cast<XmlListParser*>(self)->onEnd(name); });
  XML_SetCharacterDataHandler(
      p, [](void* self, const XML_Char* s, int len) { static_cast<XmlListParser*>(self)->onText(s, len); });
  bool ok = true;
  for (;;) {
    const int n = read(readCtx, buf, sizeof(buf));
    if (n < 0) {
      ok = false;
      break;
    }
    const bool last = n == 0;
    if (XML_Parse(p, buf, last ? 0 : n, last ? XML_TRUE : XML_FALSE) != XML_STATUS_OK || limitExceeded) {
      ok = false;
      break;
    }
    if (last) break;
  }
  XML_ParserFree(p);
  return ok;
}

void XmlListParser::onStart(const XML_Char* name, const XML_Char** atts) {
  if (limitExceeded) return;
  if (++depth > 32) {
    limitExceeded = true;
    return;
  }
  const char* local = localName(name);
  if (itemDepth < 0) {
    if (parsedItems < MAX_ITEMS && item == local) {
      itemDepth = depth;
      current = RawItem{};
      capturingMask = 0;
      for (int i = 0; i < F_COUNT; i++) {
        done[i] = !sel[i].isSet();
        if (sel[i].onItemTag) {
          const char* v = findAttr(atts, sel[i].attr);
          if (v) {
            if (strlen(v) > MAX_FIELD_CHARS) {
              limitExceeded = true;
              return;
            }
            current.field[i] = v;
          }
          done[i] = true;
        }
      }
    }
    return;
  }
  // Inside an item: a child element ends any leading-text capture.
  capturingMask = 0;
  if (!container.empty() && container == local) current.isDir = true;
  for (int i = 0; i < F_COUNT; i++) {
    if (done[i] || sel[i].onItemTag || sel[i].elem != local) continue;
    done[i] = true;  // first matching descendant wins
    if (!sel[i].attr.empty()) {
      const char* v = findAttr(atts, sel[i].attr);
      if (v) {
        if (strlen(v) > MAX_FIELD_CHARS) {
          limitExceeded = true;
          return;
        }
        current.field[i] = v;
      }
    } else {
      capturingMask |= 1u << i;
    }
  }
}

void XmlListParser::onEnd(const XML_Char* name) {
  if (limitExceeded) return;
  if (itemDepth >= 0) {
    capturingMask = 0;
    if (depth == itemDepth && item == localName(name)) {
      for (auto& f : current.field) trim(f);
      ++parsedItems;
      if (acceptRow(current)) {
        size_t bytes = 0;
        for (const auto& field : current.field) bytes += field.size();
        if (bytes > captureBudget - capturedBytes) {
          limitExceeded = true;
          return;
        }
        capturedBytes += bytes;
        sink(sinkCtx, current);
      }
      itemDepth = -1;
    }
  }
  depth--;
}

void XmlListParser::onText(const XML_Char* s, const int len) {
  if (limitExceeded || itemDepth < 0 || capturingMask == 0) return;
  for (int i = 0; i < F_COUNT; i++) {
    if (!(capturingMask & (1u << i)) || current.field[i].size() >= MAX_FIELD_CHARS) continue;
    current.field[i].append(s, std::min<size_t>(len, MAX_FIELD_CHARS - current.field[i].size()));
  }
}
