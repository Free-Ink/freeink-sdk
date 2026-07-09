// FreeInkBook — tolerant CSS subset parser and cascade.

#include "css/Css.h"

#include <string.h>

namespace freeink {
namespace book {

namespace {

constexpr uint32_t kMaxSheetBytes = 64 * 1024;

uint32_t fnv1a(const char* s, uint32_t len) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint8_t>(s[i]);
    hash *= 16777619u;
  }
  return hash;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

struct Cursor {
  const char* p;
  const char* end;

  bool done() const { return p >= end; }
  char peek() const { return p < end ? *p : '\0'; }
  void skipSpaceAndComments() {
    for (;;) {
      while (p < end && isSpace(*p)) ++p;
      if (p + 1 < end && p[0] == '/' && p[1] == '*') {
        p += 2;
        while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
        p = p + 2 <= end ? p + 2 : end;
        continue;
      }
      return;
    }
  }
};

// Parses "<number><unit>" into % of em. Returns -1 if unusable.
int32_t lengthToPct(const char* v, uint32_t len, bool* rootRelativeOut = nullptr) {
  if (rootRelativeOut != nullptr) *rootRelativeOut = false;
  // number
  uint32_t i = 0;
  bool neg = false;
  if (i < len && (v[i] == '-' || v[i] == '+')) {
    neg = v[i] == '-';
    ++i;
  }
  int32_t whole = 0;
  int32_t frac = 0;
  int32_t fracDiv = 1;
  bool any = false;
  while (i < len && v[i] >= '0' && v[i] <= '9') {
    whole = whole * 10 + (v[i] - '0');
    ++i;
    any = true;
  }
  if (i < len && v[i] == '.') {
    ++i;
    while (i < len && v[i] >= '0' && v[i] <= '9' && fracDiv < 1000) {
      frac = frac * 10 + (v[i] - '0');
      fracDiv *= 10;
      ++i;
      any = true;
    }
  }
  if (!any) return -1;
  const char* unit = v + i;
  const uint32_t unitLen = len - i;
  int32_t pct;
  if (unitLen == 0 && whole == 0 && frac == 0) {
    pct = 0;  // bare 0
  } else if (unitLen == 2 && strncmp(unit, "em", 2) == 0) {
    pct = whole * 100 + frac * 100 / fracDiv;
  } else if (unitLen == 3 && strncmp(unit, "rem", 3) == 0) {
    pct = whole * 100 + frac * 100 / fracDiv;
    if (rootRelativeOut != nullptr) *rootRelativeOut = true;
  } else if (unitLen == 1 && unit[0] == '%') {
    pct = whole + (fracDiv > 1 ? frac / fracDiv : 0);
  } else if (unitLen == 2 && strncmp(unit, "px", 2) == 0) {
    pct = (whole * 100 + frac * 100 / fracDiv) / 16;  // 16 px nominal em
  } else if (unitLen == 2 && strncmp(unit, "pt", 2) == 0) {
    pct = (whole * 100 + frac * 100 / fracDiv) / 12;  // 12 pt nominal em
  } else {
    return -1;
  }
  if (neg) pct = 0;  // negative indents/margins degrade to 0
  return pct;
}

void applyDeclaration(CssDecl* decl, const char* prop, uint32_t propLen, const char* value,
                      uint32_t valueLen) {
  // Trim value.
  while (valueLen > 0 && isSpace(value[0])) {
    ++value;
    --valueLen;
  }
  while (valueLen > 0 && isSpace(value[valueLen - 1])) --valueLen;
  if (valueLen == 0) return;

  auto propIs = [&](const char* name) {
    return propLen == strlen(name) && strncmp(prop, name, propLen) == 0;
  };
  auto valueIs = [&](const char* name) {
    return valueLen == strlen(name) && strncmp(value, name, valueLen) == 0;
  };

  if (propIs("font-size")) {
    if (valueIs("xx-small")) decl->sizePct = 58;
    else if (valueIs("x-small")) decl->sizePct = 69;
    else if (valueIs("small") || valueIs("smaller")) decl->sizePct = 87;
    else if (valueIs("medium")) decl->sizePct = 100;
    else if (valueIs("large") || valueIs("larger")) decl->sizePct = 120;
    else if (valueIs("x-large")) decl->sizePct = 150;
    else if (valueIs("xx-large")) decl->sizePct = 200;
    else {
      bool rootRelative = false;
      const int32_t pct = lengthToPct(value, valueLen, &rootRelative);
      if (pct > 0) {
        decl->sizePct = static_cast<uint16_t>(pct > 400 ? 400 : pct);
        decl->sizeRootRelative = rootRelative;
      }
    }
  } else if (propIs("font-weight")) {
    if (valueIs("bold") || valueIs("bolder")) decl->weightBold = 1;
    else if (valueIs("normal") || valueIs("lighter")) decl->weightBold = 0;
    else if (value[0] >= '1' && value[0] <= '9') decl->weightBold = value[0] >= '6' ? 1 : 0;
  } else if (propIs("font-style")) {
    if (valueIs("italic") || valueIs("oblique")) decl->styleItalic = 1;
    else if (valueIs("normal")) decl->styleItalic = 0;
  } else if (propIs("text-align")) {
    if (valueIs("left") || valueIs("start")) decl->align = TextAlign::Left;
    else if (valueIs("right") || valueIs("end")) decl->align = TextAlign::Right;
    else if (valueIs("center")) decl->align = TextAlign::Center;
    else if (valueIs("justify")) decl->align = TextAlign::Justify;
  } else if (propIs("text-indent")) {
    const int32_t pct = lengthToPct(value, valueLen);
    if (pct >= 0) decl->textIndentPct = static_cast<int16_t>(pct > 1000 ? 1000 : pct);
  } else if (propIs("margin-left")) {
    const int32_t pct = lengthToPct(value, valueLen);
    if (pct >= 0) decl->marginLeftPct = static_cast<int16_t>(pct > 1000 ? 1000 : pct);
  } else if (propIs("margin-top")) {
    const int32_t pct = lengthToPct(value, valueLen);
    if (pct >= 0) decl->marginTopPct = static_cast<int16_t>(pct > 1000 ? 1000 : pct);
  } else if (propIs("margin-bottom")) {
    const int32_t pct = lengthToPct(value, valueLen);
    if (pct >= 0) decl->marginBottomPct = static_cast<int16_t>(pct > 1000 ? 1000 : pct);
  } else if (propIs("margin")) {
    // Shorthand: top [right [bottom [left]]] — we take top and bottom.
    const char* parts[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t partLens[4] = {0, 0, 0, 0};
    uint32_t count = 0;
    uint32_t i = 0;
    while (i < valueLen && count < 4) {
      while (i < valueLen && isSpace(value[i])) ++i;
      const uint32_t start = i;
      while (i < valueLen && !isSpace(value[i])) ++i;
      if (i > start) {
        parts[count] = value + start;
        partLens[count] = i - start;
        ++count;
      }
    }
    if (count > 0) {
      const int32_t top = lengthToPct(parts[0], partLens[0]);
      const uint32_t bottomIdx = count >= 3 ? 2 : 0;
      const int32_t bottom = lengthToPct(parts[bottomIdx], partLens[bottomIdx]);
      const uint32_t leftIdx = count >= 4 ? 3 : (count >= 2 ? 1 : 0);
      const int32_t left = lengthToPct(parts[leftIdx], partLens[leftIdx]);
      if (top >= 0) decl->marginTopPct = static_cast<int16_t>(top > 1000 ? 1000 : top);
      if (bottom >= 0) decl->marginBottomPct = static_cast<int16_t>(bottom > 1000 ? 1000 : bottom);
      if (left >= 0) decl->marginLeftPct = static_cast<int16_t>(left > 1000 ? 1000 : left);
    }
  } else if (propIs("display")) {
    if (valueIs("none")) decl->displayNone = 1;
  } else if (propIs("text-decoration") || propIs("text-decoration-line")) {
    if (valueIs("underline")) decl->underline = 1;
    else if (valueIs("none")) decl->underline = 0;
  } else if (propIs("vertical-align")) {
    if (valueIs("super")) decl->vertAlign = 1;
    else if (valueIs("sub")) decl->vertAlign = 2;
    else if (valueIs("baseline")) decl->vertAlign = 0;
  }
}

// Parses the inside of a declaration block into `decl`.
void parseDeclarations(const char* text, uint32_t len, CssDecl* decl) {
  uint32_t i = 0;
  while (i < len) {
    // property
    while (i < len && (isSpace(text[i]) || text[i] == ';')) ++i;
    const uint32_t propStart = i;
    while (i < len && text[i] != ':' && text[i] != ';' && text[i] != '}') ++i;
    if (i >= len || text[i] != ':') {
      while (i < len && text[i] != ';') ++i;
      continue;
    }
    uint32_t propEnd = i;
    while (propEnd > propStart && isSpace(text[propEnd - 1])) --propEnd;
    ++i;  // ':'
    const uint32_t valueStart = i;
    while (i < len && text[i] != ';') ++i;
    applyDeclaration(decl, text + propStart, propEnd - propStart, text + valueStart,
                     i - valueStart);
  }
}

// Parses one simple selector token ("p", ".note", "p.note"). Returns false
// if it uses anything unsupported (#id, :pseudo, [attr], *…).
bool parseSimpleSelector(const char* sel, uint32_t len, uint32_t* elemHash,
                         uint32_t* classHash) {
  *elemHash = 0;
  *classHash = 0;
  if (len == 1 && sel[0] == '*') return true;
  uint32_t i = 0;
  const uint32_t elemStart = i;
  while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[') ++i;
  if (i > elemStart) *elemHash = fnv1a(sel + elemStart, i - elemStart);
  if (i < len && sel[i] == '.') {
    ++i;
    const uint32_t clsStart = i;
    while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[') ++i;
    if (i == clsStart) return false;
    *classHash = fnv1a(sel + clsStart, i - clsStart);
  }
  return i == len;  // anything left over is unsupported
}

}  // namespace

void CssDecl::applyOver(const CssDecl& over) {
  if (over.sizePct != 0) {
    sizePct = over.sizePct;
    sizeRootRelative = over.sizeRootRelative;
  }
  if (over.weightBold >= 0) weightBold = over.weightBold;
  if (over.styleItalic >= 0) styleItalic = over.styleItalic;
  if (over.align != TextAlign::Inherit) align = over.align;
  if (over.textIndentPct >= 0) textIndentPct = over.textIndentPct;
  if (over.marginLeftPct >= 0) marginLeftPct = over.marginLeftPct;
  if (over.marginTopPct >= 0) marginTopPct = over.marginTopPct;
  if (over.marginBottomPct >= 0) marginBottomPct = over.marginBottomPct;
  if (over.displayNone >= 0) displayNone = over.displayNone;
  if (over.underline >= 0) underline = over.underline;
  if (over.vertAlign >= 0) vertAlign = over.vertAlign;
}

bool CssStylesheetBuilder::begin(Arena& arena) {
  arena_ = &arena;
  rules_ = arena.allocArray<CssRule>(kMaxRules);
  ruleCount_ = 0;
  skippedSheets_ = 0;
  contentHash_ = 2166136261u;
  return rules_ != nullptr;
}

void CssStylesheetBuilder::addText(const char* css, uint32_t len) {
  if (rules_ == nullptr) return;  // begin() failed — stay inert
  for (uint32_t i = 0; i < len; ++i) {
    contentHash_ ^= static_cast<uint8_t>(css[i]);
    contentHash_ *= 16777619u;
  }

  Cursor cur{css, css + len};
  for (;;) {
    cur.skipSpaceAndComments();
    if (cur.done()) return;

    // Skip at-rules wholesale (@media blocks: skip the nested braces).
    if (cur.peek() == '@') {
      int depth = 0;
      bool sawBrace = false;
      while (!cur.done()) {
        const char c = *cur.p++;
        if (c == '{') {
          ++depth;
          sawBrace = true;
        } else if (c == '}') {
          if (--depth <= 0) break;
        } else if (c == ';' && !sawBrace) {
          break;
        }
      }
      continue;
    }

    // Selector list up to '{'.
    const char* selStart = cur.p;
    while (!cur.done() && cur.peek() != '{') ++cur.p;
    if (cur.done()) return;
    const char* selEnd = cur.p;
    ++cur.p;  // '{'
    const char* declStart = cur.p;
    while (!cur.done() && cur.peek() != '}') ++cur.p;
    const char* declEnd = cur.p;
    if (!cur.done()) ++cur.p;  // '}'

    CssDecl decl;
    parseDeclarations(declStart, static_cast<uint32_t>(declEnd - declStart), &decl);

    // One rule per selector in the comma list; the rightmost simple selector
    // of each descendant chain is what we match.
    const char* s = selStart;
    while (s < selEnd) {
      const char* comma = static_cast<const char*>(memchr(s, ',', selEnd - s));
      const char* end = comma != nullptr ? comma : selEnd;
      // Trim and take the last whitespace-separated token.
      const char* tokEnd = end;
      while (tokEnd > s && isSpace(tokEnd[-1])) --tokEnd;
      const char* tokStart = tokEnd;
      while (tokStart > s && !isSpace(tokStart[-1]) && tokStart[-1] != '>' &&
             tokStart[-1] != '+' && tokStart[-1] != '~') {
        --tokStart;
      }
      uint32_t elemHash = 0;
      uint32_t classHash = 0;
      if (tokEnd > tokStart &&
          parseSimpleSelector(tokStart, static_cast<uint32_t>(tokEnd - tokStart), &elemHash,
                              &classHash) &&
          ruleCount_ < kMaxRules) {
        rules_[ruleCount_++] = CssRule{elemHash, classHash, decl};
      }
      s = comma != nullptr ? comma + 1 : selEnd;
    }
  }
}

BookStatus CssStylesheetBuilder::addSheet(BookSource& source, const ZipEntry& entry,
                                          Arena& scratch) {
  if (entry.uncompressedSize > kMaxSheetBytes) {
    ++skippedSheets_;
    return BookStatus::Ok;
  }
  const size_t marked = scratch.mark();
  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) {
    scratch.release(marked);
    return status;
  }
  char* buf = static_cast<char*>(scratch.alloc(entry.uncompressedSize + 1, 1));
  if (buf == nullptr) {
    scratch.release(marked);
    return BookStatus::OutOfMemory;
  }
  uint32_t total = 0;
  while (total < entry.uncompressedSize) {
    const int32_t n = reader.read(buf + total, entry.uncompressedSize - total);
    if (n < 0) {
      scratch.release(marked);
      return BookStatus::IoError;
    }
    if (n == 0) break;
    total += static_cast<uint32_t>(n);
  }
  addText(buf, total);
  scratch.release(marked);
  return BookStatus::Ok;
}

CssStylesheet CssStylesheetBuilder::finish() {
  CssStylesheet sheet;
  sheet.rules = rules_;
  sheet.ruleCount = ruleCount_;
  sheet.contentHash = contentHash_;
  return sheet;
}

CssDecl parseInlineStyle(const char* text) {
  CssDecl decl;
  if (text != nullptr) parseDeclarations(text, static_cast<uint32_t>(strlen(text)), &decl);
  return decl;
}

CssDecl cascadeFor(const CssStylesheet& sheet, const char* elementLocalName,
                   const char* classAttr, const CssDecl* inlineDecl) {
  const uint32_t elemHash =
      elementLocalName != nullptr
          ? fnv1a(elementLocalName, static_cast<uint32_t>(strlen(elementLocalName)))
          : 0;

  // Hash each class token once (books rarely use more than a few).
  uint32_t classHashes[8];
  uint32_t classCount = 0;
  if (classAttr != nullptr) {
    const char* p = classAttr;
    while (*p != '\0' && classCount < 8) {
      while (*p != '\0' && isSpace(*p)) ++p;
      const char* start = p;
      while (*p != '\0' && !isSpace(*p)) ++p;
      if (p > start) classHashes[classCount++] = fnv1a(start, static_cast<uint32_t>(p - start));
    }
  }

  auto ruleMatches = [&](const CssRule& rule) {
    if (rule.elemHash != 0 && rule.elemHash != elemHash) return false;
    if (rule.classHash != 0) {
      for (uint32_t c = 0; c < classCount; ++c) {
        if (classHashes[c] == rule.classHash) return true;
      }
      return false;
    }
    return rule.elemHash != 0 || rule.classHash != 0;  // reject the empty selector
  };

  CssDecl out;
  // Specificity passes: element-only, class-only, element+class, inline.
  for (int pass = 0; pass < 3; ++pass) {
    for (uint16_t r = 0; r < sheet.ruleCount; ++r) {
      const CssRule& rule = sheet.rules[r];
      const int spec = rule.classHash != 0 ? (rule.elemHash != 0 ? 2 : 1) : 0;
      if (spec != pass || !ruleMatches(rule)) continue;
      out.applyOver(rule.decl);
    }
  }
  if (inlineDecl != nullptr) out.applyOver(*inlineDecl);
  return out;
}

}  // namespace book
}  // namespace freeink
