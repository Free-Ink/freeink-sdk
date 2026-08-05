#pragma once

// FreeInk — XML tree model, serializer, and the rights
// request-signing canonicalization (SHA-1 over a custom binary serialization
// — namespace URIs inlined, attributes sorted, text trimmed, length-prefixed
// strings). Must match the server's canonicalization byte-for-byte.
//
// Cross-validated against a reference implementation (TypeScript).
// Limitation: mixed content (text interleaved with elements) is not modeled;
// rights documents are never mixed-content.
//
// Freestanding C++17 (header-only).

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Crypto.h"
#include "Xml.h"

namespace freeink {
namespace content {

struct XmlTNode {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;  // insertion order
  std::vector<XmlTNode> children;                          // element children
  std::string text;                                        // pcdata (decoded)

  const char* attr(const char* name) const {
    for (const auto& a : attrs) {
      if (a.first == name) return a.second.c_str();
    }
    return nullptr;
  }
  void setAttr(const std::string& name, const std::string& value) {
    for (auto& a : attrs) {
      if (a.first == name) {
        a.second = value;
        return;
      }
    }
    attrs.emplace_back(name, value);
  }
  const XmlTNode* child(const char* name) const {
    for (const auto& c : children) {
      if (c.name == name || (c.name.size() > strlen(name) &&
                             c.name.compare(c.name.size() - strlen(name), strlen(name), name) == 0 &&
                             c.name[c.name.size() - strlen(name) - 1] == ':')) {
        return &c;
      }
    }
    return nullptr;
  }
  XmlTNode* child(const char* name) {
    return const_cast<XmlTNode*>(static_cast<const XmlTNode*>(this)->child(name));
  }
  const XmlTNode* find(const char* name) const {
    if (name == this->name || (this->name.size() > strlen(name) &&
                               this->name.compare(this->name.size() - strlen(name), strlen(name), name) ==
                                   0 &&
                               this->name[this->name.size() - strlen(name) - 1] == ':')) {
      return this;
    }
    for (const auto& c : children) {
      if (const XmlTNode* hit = c.find(name)) return hit;
    }
    return nullptr;
  }
  XmlTNode* find(const char* name) {
    return const_cast<XmlTNode*>(static_cast<const XmlTNode*>(this)->find(name));
  }
  std::string childText(const char* name) const {
    const XmlTNode* c = child(name);
    return c ? c->text : std::string();
  }
};

inline XmlTNode elem(const std::string& name,
                     std::vector<std::pair<std::string, std::string>> attrs = {},
                     std::vector<XmlTNode> children = {}) {
  XmlTNode n;
  n.name = name;
  n.attrs = std::move(attrs);
  n.children = std::move(children);
  return n;
}

inline XmlTNode textElem(const std::string& name, const std::string& text) {
  XmlTNode n;
  n.name = name;
  n.text = text;
  return n;
}

// --- parser (scanner events -> tree) ---------------------------------------

inline bool parseXmlTree(const std::string& xml, XmlTNode* out) {
  XmlScanner scanner(xml);
  std::vector<XmlTNode*> stack;
  XmlTNode root;
  for (;;) {
    XmlScanner::Event ev = scanner.nextEvent();
    if (ev.type == XmlScanner::Event::DONE) break;
    if (ev.type == XmlScanner::Event::ERROR) return false;

    if (ev.type == XmlScanner::Event::START) {
      XmlTNode node;
      node.name = ev.name;
      for (const auto& a : ev.attrs) node.attrs.emplace_back(a.name, a.value);
      if (stack.empty()) {
        root = std::move(node);
        stack.push_back(&root);
      } else {
        stack.back()->children.push_back(std::move(node));
        stack.push_back(&stack.back()->children.back());
      }
      continue;
    }
    if (ev.type == XmlScanner::Event::END) {
      if (!stack.empty()) stack.pop_back();
      continue;
    }
    // TEXT
    if (!stack.empty()) stack.back()->text += ev.text;
  }
  *out = std::move(root);
  return true;
}

// --- serializer --------------------------------------------------------------

inline void serializeInto(const XmlTNode& node, std::string* out) {
  *out += '<';
  *out += node.name;
  for (const auto& a : node.attrs) {
    *out += ' ';
    *out += a.first;
    *out += "=\"";
    for (const char c : a.second) {
      switch (c) {
        case '&': *out += "&amp;"; break;
        case '<': *out += "&lt;"; break;
        case '>': *out += "&gt;"; break;
        case '"': *out += "&quot;"; break;
        default: *out += c;
      }
    }
    *out += '"';
  }
  if (node.children.empty() && node.text.empty()) {
    *out += "/>";
    return;
  }
  *out += '>';
  for (const char c : node.text) {
    switch (c) {
      case '&': *out += "&amp;"; break;
      case '<': *out += "&lt;"; break;
      case '>': *out += "&gt;"; break;
      default: *out += c;
    }
  }
  for (const auto& c : node.children) serializeInto(c, out);
  *out += "</";
  *out += node.name;
  *out += '>';
}

inline std::string serializeXml(const XmlTNode& node) {
  std::string out = "<?xml version=\"1.0\"?>\n";
  serializeInto(node, &out);
  return out;
}

// --- signing canonicalization -------------------------------------------------

namespace canon {
constexpr uint8_t ASN_NS_TAG = 0x01;
constexpr uint8_t ASN_CHILD = 0x02;
constexpr uint8_t ASN_END_TAG = 0x03;
constexpr uint8_t ASN_TEXT = 0x04;
constexpr uint8_t ASN_ATTRIBUTE = 0x05;

struct ByteSink {
  virtual ~ByteSink() = default;
  virtual void put(const uint8_t* data, size_t len) = 0;
};

inline void pushTag(ByteSink& sink, uint8_t tag) { sink.put(&tag, 1); }

inline void pushString(ByteSink& sink, const std::string& s) {
  const uint8_t len[2] = {static_cast<uint8_t>((s.size() >> 8) & 0xFF),
                          static_cast<uint8_t>(s.size() & 0xFF)};
  sink.put(len, 2);
  sink.put(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline std::string trimText(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

inline void walk(const XmlTNode& node, std::vector<std::pair<std::string, std::string>>& nsHash,
                 ByteSink& sink) {
  // Register namespace declarations (single shared map for the whole walk).
  for (const auto& a : node.attrs) {
    if (a.first == "xmlns") nsHash.emplace_back("GENERICNS", a.second);
    else if (a.first.rfind("xmlns:", 0) == 0) nsHash.emplace_back(a.first.substr(6), a.second);
  }

  std::string name = node.name;
  const size_t colon = name.find(':');
  if (colon != std::string::npos) {
    const std::string prefix = name.substr(0, colon);
    std::string uri;
    for (auto it = nsHash.rbegin(); it != nsHash.rend(); ++it) {
      if (it->first == prefix) {
        uri = it->second;
        break;
      }
    }
    pushTag(sink, ASN_NS_TAG);
    pushString(sink, uri);
    name = name.substr(colon + 1);
  } else {
    std::string uri;
    for (auto it = nsHash.rbegin(); it != nsHash.rend(); ++it) {
      if (it->first == "GENERICNS") {
        uri = it->second;
        break;
      }
    }
    if (!uri.empty()) {
      pushTag(sink, ASN_NS_TAG);
      pushString(sink, uri);
    }
  }
  pushString(sink, name);

  std::vector<const std::pair<std::string, std::string>*> attrs;
  for (const auto& a : node.attrs) {
    if (a.first.find("xmlns") != std::string::npos) continue;
    attrs.push_back(&a);
  }
  std::sort(attrs.begin(), attrs.end(),
            [](const auto* a, const auto* b) { return a->first < b->first; });
  for (const auto* a : attrs) {
    pushTag(sink, ASN_ATTRIBUTE);
    pushString(sink, "");
    pushString(sink, a->first);
    pushString(sink, a->second);
  }

  pushTag(sink, ASN_CHILD);
  const std::string text = trimText(node.text);
  if (!text.empty()) {
    pushTag(sink, ASN_TEXT);
    pushString(sink, text);
  }
  for (const auto& c : node.children) walk(c, nsHash, sink);
  pushTag(sink, ASN_END_TAG);
}

// Accumulating sink that feeds Crypto::sha1 in one pass (small docs).
struct BufferSink : ByteSink {
  std::vector<uint8_t> buf;
  void put(const uint8_t* data, size_t len) override { buf.insert(buf.end(), data, data + len); }
};
}  // namespace canon

// SHA-1 of the canonicalized node — the signature input.
inline void canonHash(const XmlTNode& node, Crypto& crypto, uint8_t out[20]) {
  canon::BufferSink sink;
  std::vector<std::pair<std::string, std::string>> nsHash;
  canon::walk(node, nsHash, sink);
  crypto.sha1(sink.buf.data(), sink.buf.size(), out);
}

}  // namespace content
}  // namespace freeink
