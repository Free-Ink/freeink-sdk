#pragma once

// FreeInk — minimal XML scanner (header-only).
//
// rights.xml and encryption.xml are small, stable schemas. A full parser
// (expat) would be overkill here and costs a vendored dependency; this
// scanner emits start-tag / end-tag / text events with attributes, matches
// element names with or without namespace prefix, and decodes entities.
// Not a general-purpose XML parser: no DTD, no mixed-content fidelity — but
// sufficient and safe for rights metadata documents.
//
// Freestanding C++17.

#include <string.h>

#include <string>
#include <vector>

namespace freeink {
namespace content {

struct XmlAttr {
  std::string name;
  std::string value;
};

class XmlScanner {
 public:
  struct Event {
    enum Type { START, END, TEXT, DONE, ERROR } type = DONE;
    std::string name;                // START/END: element name (prefix kept)
    std::vector<XmlAttr> attrs;      // START only
    std::string text;                // TEXT only (entity-decoded)
  };

  explicit XmlScanner(const std::string& doc) : doc_(doc) {}

  Event next() {
    Event ev;
    for (;;) {
      if (pos_ >= doc_.size()) {
        ev.type = stackEmpty() ? Event::DONE : Event::ERROR;
        return ev;
      }
      if (doc_[pos_] != '<') {
        // Text run.
        const size_t end = doc_.find('<', pos_);
        const size_t stop = (end == std::string::npos) ? doc_.size() : end;
        ev.type = Event::TEXT;
        ev.text = decode(doc_.substr(pos_, stop - pos_));
        pos_ = stop;
        return ev;
      }
      if (doc_.compare(pos_, 2, "<?") == 0) {
        pos_ = skipPast("?>", pos_);
        continue;
      }
      if (doc_.compare(pos_, 4, "<!--") == 0) {
        pos_ = skipPast("-->", pos_);
        continue;
      }
      if (doc_.compare(pos_, 9, "<![CDATA[") == 0) {
        const size_t end = doc_.find("]]>", pos_);
        const size_t stop = (end == std::string::npos) ? doc_.size() : end;
        ev.type = Event::TEXT;
        ev.text = doc_.substr(pos_ + 9, stop - (pos_ + 9));
        pos_ = (end == std::string::npos) ? doc_.size() : end + 3;
        return ev;
      }
      if (doc_.compare(pos_, 2, "<!") == 0) {
        pos_ = skipPast(">", pos_);
        continue;
      }
      if (doc_.compare(pos_, 2, "</") == 0) {
        const size_t end = doc_.find('>', pos_);
        if (end == std::string::npos) {
          ev.type = Event::ERROR;
          return ev;
        }
        ev.type = Event::END;
        ev.name = doc_.substr(pos_ + 2, end - (pos_ + 2));
        pos_ = end + 1;
        if (!stack_.empty()) stack_.pop_back();
        return ev;
      }
      // Start tag (or self-closing).
      const size_t end = findTagEnd(pos_);
      if (end == std::string::npos) {
        ev.type = Event::ERROR;
        return ev;
      }
      std::string content = doc_.substr(pos_ + 1, end - pos_ - 1);
      const bool selfClose = !content.empty() && content.back() == '/';
      if (selfClose) content.pop_back();
      parseTag(content, &ev);
      if (selfClose) {
        // Emit START; a synthetic END follows on the next next() call.
        pendingEnds_.push_back(ev.name);
      } else {
        stack_.push_back(ev.name);
      }
      pos_ = end + 1;
      return ev;
    }
  }

  // Delivers synthetic END events for self-closed tags. Call instead of
  // next() when draining the stream.
  Event nextEvent() {
    if (!pendingEnds_.empty()) {
      Event ev;
      ev.type = Event::END;
      ev.name = pendingEnds_.back();
      pendingEnds_.pop_back();
      return ev;
    }
    return next();
  }

  // Name match with or without namespace prefix.
  static bool named(const std::string& actual, const char* want) {
    if (actual == want) return true;
    const size_t wantLen = strlen(want);
    return actual.size() > wantLen && actual[actual.size() - wantLen - 1] == ':' &&
           actual.compare(actual.size() - wantLen, wantLen, want) == 0;
  }

 private:
  size_t skipPast(const char* marker, size_t from) {
    const size_t end = doc_.find(marker, from);
    return (end == std::string::npos) ? doc_.size() : end + strlen(marker);
  }

  size_t findTagEnd(size_t start) const {
    char quote = 0;
    for (size_t i = start + 1; i < doc_.size(); i++) {
      const char c = doc_[i];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '>') {
        return i;
      }
    }
    return std::string::npos;
  }

  void parseTag(const std::string& content, Event* ev) {
    ev->type = Event::START;
    const size_t sp = content.find_first_of(" \t\r\n");
    ev->name = (sp == std::string::npos) ? content : content.substr(0, sp);
    size_t i = (sp == std::string::npos) ? content.size() : sp;
    while (i < content.size()) {
      while (i < content.size() && isspace(static_cast<unsigned char>(content[i]))) i++;
      if (i >= content.size()) break;
      const size_t eq = content.find('=', i);
      if (eq == std::string::npos) break;
      std::string attrName = content.substr(i, eq - i);
      // Trim trailing whitespace from the name.
      while (!attrName.empty() && isspace(static_cast<unsigned char>(attrName.back()))) attrName.pop_back();
      i = eq + 1;
      while (i < content.size() && isspace(static_cast<unsigned char>(content[i]))) i++;
      if (i >= content.size() || (content[i] != '"' && content[i] != '\'')) break;
      const char q = content[i++];
      const size_t vEnd = content.find(q, i);
      XmlAttr attr{attrName, decode(content.substr(i, (vEnd == std::string::npos ? content.size() : vEnd) - i))};
      ev->attrs.push_back(std::move(attr));
      i = (vEnd == std::string::npos) ? content.size() : vEnd + 1;
    }
  }

  static std::string decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
      if (in[i] == '&') {
        const size_t semi = in.find(';', i);
        if (semi != std::string::npos) {
          const std::string ent = in.substr(i + 1, semi - i - 1);
          if (ent == "amp") out += '&';
          else if (ent == "lt") out += '<';
          else if (ent == "gt") out += '>';
          else if (ent == "quot") out += '"';
          else if (ent == "apos") out += '\'';
          else if (!ent.empty() && ent[0] == '#') {
            const long code = strtol(ent.c_str() + (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? 2 : 1),
                                     nullptr, (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? 16 : 10);
            if (code > 0 && code < 0x80) out += static_cast<char>(code);
            // Non-ASCII code points are pass-through ignored; rights metadata
            // here is ASCII (URLs, UUIDs, base64).
          } else {
            out += '&';
            out += ent;
            out += ';';
          }
          i = semi;
          continue;
        }
      }
      out += in[i];
    }
    return out;
  }

  bool stackEmpty() const { return stack_.empty(); }

  const std::string& doc_;
  size_t pos_ = 0;
  std::vector<std::string> stack_;
  std::vector<std::string> pendingEnds_;
};

}  // namespace content
}  // namespace freeink
