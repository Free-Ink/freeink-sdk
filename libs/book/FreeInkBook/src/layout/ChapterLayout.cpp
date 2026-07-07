// FreeInkBook — streaming SAX → block flow → lines → pages.

#include "layout/ChapterLayout.h"

#include <string.h>

#include <linebreak.h>

#include "epub/XmlSax.h"

namespace freeink {
namespace book {

namespace {

// Fixed working-set sizes. These bound layout memory regardless of chapter
// size; a paragraph longer than the buffer is flushed in segments (a layout
// artifact for pathological paragraphs, never a failure).
constexpr uint32_t kParTextCap = 8192;
constexpr uint16_t kMaxSpans = 128;
constexpr uint16_t kMaxRunsPerPage = 384;
constexpr uint32_t kPageArenaCap = 24 * 1024;

const char* localName(const char* qname) {
  const char* colon = strrchr(qname, ':');
  return colon != nullptr ? colon + 1 : qname;
}

// Counts UTF-8 codepoints in text[0..len).
uint32_t countChars(const char* text, uint32_t len) {
  uint32_t chars = 0;
  for (uint32_t i = 0; i < len; ++i) {
    if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) ++chars;
  }
  return chars;
}

// Decodes the UTF-8 codepoint starting at text[i]; advances i past it.
uint32_t decodeUtf8(const char* text, uint32_t len, uint32_t& i) {
  const uint8_t b0 = static_cast<uint8_t>(text[i]);
  uint32_t cp = b0;
  uint32_t extra = 0;
  if (b0 >= 0xF0) {
    cp = b0 & 0x07;
    extra = 3;
  } else if (b0 >= 0xE0) {
    cp = b0 & 0x0F;
    extra = 2;
  } else if (b0 >= 0xC0) {
    cp = b0 & 0x1F;
    extra = 1;
  }
  ++i;
  while (extra > 0 && i < len && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) {
    cp = (cp << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
    ++i;
    --extra;
  }
  return cp;
}

struct BlockStyle {
  uint16_t scalePct;      // font size as % of base
  int16_t indentPx;       // left indent in units of baseSizePx/16
  uint16_t spaceBeforePct;  // % of resolved size
  uint16_t spaceAfterPct;
  uint8_t styleFlags;     // headings render bold
};

BlockStyle blockStyleFor(const char* local, uint16_t /*baseSizePx*/) {
  if (strcmp(local, "h1") == 0) return {200, 0, 100, 50, StyleBold};
  if (strcmp(local, "h2") == 0) return {160, 0, 90, 45, StyleBold};
  if (strcmp(local, "h3") == 0) return {130, 0, 80, 40, StyleBold};
  if (strcmp(local, "h4") == 0 || strcmp(local, "h5") == 0 || strcmp(local, "h6") == 0) {
    return {115, 0, 70, 35, StyleBold};
  }
  if (strcmp(local, "blockquote") == 0) return {100, 32, 40, 40, StyleItalic};
  if (strcmp(local, "li") == 0) return {100, 24, 10, 10, StyleNone};
  return {100, 0, 0, 40, StyleNone};  // p, div, and friends
}

bool isBlockElement(const char* local) {
  static const char* kBlocks[] = {"p",       "h1",      "h2",         "h3",    "h4",
                                  "h5",      "h6",      "blockquote", "li",    "div",
                                  "section", "article", "figure",     "aside", "figcaption",
                                  "ul",      "ol",      "table",      "tr",    "td",
                                  "th",      "dt",      "dd"};
  for (const char* b : kBlocks) {
    if (strcmp(local, b) == 0) return true;
  }
  return false;
}

bool isSuppressedElement(const char* local) {
  return strcmp(local, "head") == 0 || strcmp(local, "style") == 0 ||
         strcmp(local, "script") == 0 || strcmp(local, "title") == 0;
}

class LayoutEngine : public XmlHandler {
 public:
  LayoutEngine(const LayoutParams& params, Arena& scratch, PageSink& sink)
      : params_(params), scratch_(scratch), sink_(sink) {}

  bool init() {
    parText_ = static_cast<char*>(scratch_.alloc(kParTextCap, 1));
    breaks_ = static_cast<char*>(scratch_.alloc(kParTextCap, 1));
    spans_ = scratch_.allocArray<Span>(kMaxSpans);
    runs_ = scratch_.allocArray<PageTextRun>(kMaxRunsPerPage);
    // Page-run text lives in its own sub-arena, NOT in `scratch_`: the XML
    // parse that drives this engine keeps its inflate window and parser
    // buffers live in `scratch_` for the whole chapter, and those are
    // allocated *after* init() runs. Releasing per-page copies from the
    // shared arena would free the decompressor out from under the reader.
    void* pageBlock = scratch_.alloc(kPageArenaCap, alignof(max_align_t));
    if (parText_ == nullptr || breaks_ == nullptr || spans_ == nullptr || runs_ == nullptr ||
        pageBlock == nullptr) {
      return false;
    }
    pageArena_.init(pageBlock, kPageArenaCap);
    pageY_ = params_.marginTop;
    beginParagraph(blockStyleFor("p", params_.baseSizePx));
    return true;
  }

  // --- XmlHandler ----------------------------------------------------------

  void onStartElement(const char* name, const char** atts) override {
    (void)atts;
    if (failed_ || stopParse) return;
    const char* local = localName(name);
    if (isSuppressedElement(local)) {
      ++suppress_;
      return;
    }
    if (strcmp(local, "body") == 0) {
      inBody_ = true;
      return;
    }
    if (!inBody_ || suppress_ > 0) return;

    if (isBlockElement(local)) {
      flushParagraph();
      beginParagraph(blockStyleFor(local, params_.baseSizePx));
    } else if (strcmp(local, "br") == 0) {
      appendRaw('\n');
    } else if (strcmp(local, "hr") == 0) {
      flushParagraph();
      advanceY(lineHeightFor(params_.baseSizePx));  // blank band; drawn rule in Phase 4
    } else if (strcmp(local, "b") == 0 || strcmp(local, "strong") == 0) {
      ++bold_;
      noteStyleChange();
    } else if (strcmp(local, "i") == 0 || strcmp(local, "em") == 0) {
      ++italic_;
      noteStyleChange();
    }
  }

  void onEndElement(const char* name) override {
    if (failed_ || stopParse) return;
    const char* local = localName(name);
    if (isSuppressedElement(local)) {
      if (suppress_ > 0) --suppress_;
      return;
    }
    if (strcmp(local, "body") == 0) {
      flushParagraph();
      inBody_ = false;
      return;
    }
    if (!inBody_ || suppress_ > 0) return;

    if (isBlockElement(local)) {
      flushParagraph();
      beginParagraph(blockStyleFor("p", params_.baseSizePx));
    } else if (strcmp(local, "b") == 0 || strcmp(local, "strong") == 0) {
      if (bold_ > 0) --bold_;
      noteStyleChange();
    } else if (strcmp(local, "i") == 0 || strcmp(local, "em") == 0) {
      if (italic_ > 0) --italic_;
      noteStyleChange();
    }
  }

  void onText(const char* text, int len) override {
    if (failed_ || stopParse || !inBody_ || suppress_ > 0) return;
    for (int i = 0; i < len; ++i) {
      const char c = text[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        pendingSpace_ = parLen_ > 0;  // collapse runs; drop leading whitespace
      } else {
        if (pendingSpace_) {
          appendRaw(' ');
          pendingSpace_ = false;
        }
        appendRaw(c);
      }
    }
  }

  // --- Completion ----------------------------------------------------------

  bool finish() {
    if (failed_) return false;
    flushParagraph();
    if (runCount_ > 0) emitPage();
    return !failed_;
  }

  uint32_t pageCount() const { return pageCount_; }
  bool outOfMemory() const { return failed_; }

 private:
  struct Span {
    uint16_t start;
    uint8_t flags;
  };

  uint8_t currentFlags() const {
    uint8_t flags = paragraphStyle_.styleFlags;
    if (bold_ > 0) flags |= StyleBold;
    if (italic_ > 0) flags |= StyleItalic;
    return flags;
  }

  void noteStyleChange() {
    if (spanCount_ == 0) return;  // paragraph text not started yet
    const uint8_t flags = currentFlags();
    if (spans_[spanCount_ - 1].flags == flags) return;
    if (spans_[spanCount_ - 1].start == parLen_) {
      spans_[spanCount_ - 1].flags = flags;  // empty span — overwrite
    } else if (spanCount_ < kMaxSpans) {
      spans_[spanCount_++] = {static_cast<uint16_t>(parLen_), flags};
    }
  }

  void appendRaw(char c) {
    if (parLen_ + 1 >= kParTextCap) {
      // Pathological paragraph: flush what we have and continue seamlessly.
      const BlockStyle style = paragraphStyle_;
      flushParagraph();
      beginParagraph(style);
    }
    if (spanCount_ == 0) {
      spans_[spanCount_++] = {0, currentFlags()};
    }
    parText_[parLen_++] = c;
  }

  void beginParagraph(const BlockStyle& style) {
    paragraphStyle_ = style;
    parLen_ = 0;
    spanCount_ = 0;
    pendingSpace_ = false;
  }

  uint16_t resolvedSize() const {
    const uint32_t size = static_cast<uint32_t>(params_.baseSizePx) * paragraphStyle_.scalePct / 100;
    return static_cast<uint16_t>(size < 8 ? 8 : size);
  }

  int16_t lineHeightFor(uint16_t sizePx) const { return params_.font->lineHeight(sizePx); }

  uint8_t flagsAt(uint32_t byteOffset) const {
    uint8_t flags = spans_[0].flags;
    for (uint16_t s = 0; s < spanCount_ && spans_[s].start <= byteOffset; ++s) {
      flags = spans_[s].flags;
    }
    return flags;
  }

  void advanceY(int16_t dy) {
    if (pageY_ > params_.marginTop) pageY_ += dy;  // no leading gaps at page top
  }

  void flushParagraph() {
    if (parLen_ == 0) return;
    const uint16_t sizePx = resolvedSize();
    const int16_t lineHeight = lineHeightFor(sizePx);
    advanceY(static_cast<int16_t>(lineHeight * paragraphStyle_.spaceBeforePct / 100));

    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(parText_), parLen_, params_.language,
                        breaks_);

    const int16_t maxWidth = params_.pageWidth - params_.marginLeft - params_.marginRight -
                             paragraphStyle_.indentPx;
    uint32_t lineStart = 0;
    uint32_t i = 0;
    int32_t lineWidth = 0;
    uint32_t lastBreakEnd = 0;  // byte end of the last allowed break, 0 = none

    while (i < parLen_ && !failed_ && !stopParse) {
      const uint32_t charStart = i;
      const uint32_t cp = decodeUtf8(parText_, parLen_, i);
      const uint8_t flags = flagsAt(charStart);
      const int32_t adv =
          cp == '\n' ? 0 : params_.font->advance(cp, sizePx, flags);

      if (cp != '\n' && lineWidth + adv > maxWidth && charStart > lineStart) {
        // Break at the last opportunity, or force mid-word if there was none.
        const uint32_t breakEnd = lastBreakEnd > lineStart ? lastBreakEnd : charStart;
        emitLine(lineStart, breakEnd, sizePx, lineHeight);
        lineStart = breakEnd;
        while (lineStart < parLen_ && parText_[lineStart] == ' ') ++lineStart;
        i = lineStart;
        lineWidth = 0;
        lastBreakEnd = 0;
        continue;
      }
      lineWidth += adv;

      const char brk = breaks_[i - 1];  // opportunity after this character
      if (brk == LINEBREAK_MUSTBREAK) {
        emitLine(lineStart, i, sizePx, lineHeight);
        lineStart = i;
        lineWidth = 0;
        lastBreakEnd = 0;
      } else if (brk == LINEBREAK_ALLOWBREAK) {
        lastBreakEnd = i;
      }
    }
    if (lineStart < parLen_ && !failed_ && !stopParse) {
      emitLine(lineStart, parLen_, sizePx, lineHeight);
    }

    advanceY(static_cast<int16_t>(lineHeight * paragraphStyle_.spaceAfterPct / 100));
    parCharBase_ += countChars(parText_, parLen_);
    parLen_ = 0;
    spanCount_ = 0;
    pendingSpace_ = false;
  }

  void emitLine(uint32_t start, uint32_t end, uint16_t sizePx, int16_t lineHeight) {
    // Trim what a rendered line never shows.
    while (end > start && (parText_[end - 1] == ' ' || parText_[end - 1] == '\n')) --end;

    if (pageY_ + lineHeight > params_.pageHeight - params_.marginBottom) emitPage();
    if (failed_ || stopParse) return;

    if (end == start) {  // blank line (e.g. double <br/>)
      pageY_ += lineHeight;
      return;
    }

    if (runCount_ == 0) {
      pageCharStart_ = parCharBase_ + countChars(parText_, start);
    }

    int16_t baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(sizePx));
    int16_t x = static_cast<int16_t>(params_.marginLeft + paragraphStyle_.indentPx);

    // Slice the line into same-style runs.
    uint32_t segStart = start;
    while (segStart < end && !failed_) {
      if (runCount_ >= kMaxRunsPerPage) {
        // Pathologically dense page — flush it and continue the line on a
        // fresh one rather than dropping content.
        emitPage();
        if (stopParse) return;
        baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(sizePx));
      }

      const uint8_t flags = flagsAt(segStart);
      uint32_t segEnd = segStart;
      int32_t segWidth = 0;
      while (segEnd < end) {
        const uint32_t charStart = segEnd;
        if (flagsAt(charStart) != flags) break;
        const uint32_t cp = decodeUtf8(parText_, end, segEnd);
        segWidth += params_.font->advance(cp, sizePx, flags);
      }

      const uint32_t segLen = segEnd - segStart;
      char* copy = static_cast<char*>(pageArena_.alloc(segLen, 1));
      if (copy == nullptr) {
        // Page text arena full — flush the page and continue on a fresh one.
        emitPage();
        if (stopParse) return;
        baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(sizePx));
        copy = static_cast<char*>(pageArena_.alloc(segLen, 1));
        if (copy == nullptr) {
          failed_ = true;
          return;
        }
      }
      memcpy(copy, parText_ + segStart, segLen);
      runs_[runCount_++] = {copy,
                            static_cast<uint16_t>(segLen),
                            x,
                            baselineY,
                            sizePx,
                            flags};
      x = static_cast<int16_t>(x + segWidth);
      segStart = segEnd;
    }
    pageY_ += lineHeight;
  }

  void emitPage() {
    Page page{runs_, runCount_, pageCount_, pageCharStart_};
    ++pageCount_;
    if (!sink_.onPage(page)) stopParse = true;
    runCount_ = 0;
    pageArena_.reset();
    pageY_ = params_.marginTop;
  }

  const LayoutParams& params_;
  Arena& scratch_;
  PageSink& sink_;

  char* parText_ = nullptr;
  char* breaks_ = nullptr;
  Span* spans_ = nullptr;
  PageTextRun* runs_ = nullptr;
  Arena pageArena_;

  uint32_t parLen_ = 0;
  uint16_t spanCount_ = 0;
  BlockStyle paragraphStyle_{100, 0, 0, 40, StyleNone};
  bool pendingSpace_ = false;

  int suppress_ = 0;
  bool inBody_ = false;
  int bold_ = 0;
  int italic_ = 0;

  uint16_t runCount_ = 0;
  int16_t pageY_ = 0;
  uint32_t pageCount_ = 0;
  uint32_t parCharBase_ = 0;    // chapter chars before the current paragraph
  uint32_t pageCharStart_ = 0;  // anchor of the page being assembled
  bool failed_ = false;
};

}  // namespace

BookStatus ChapterLayout::layout(BookSource& source, const ZipEntry& entry,
                                 const LayoutParams& params, Arena& scratch, PageSink& sink,
                                 uint32_t* pageCountOut) {
  if (params.font == nullptr || params.pageWidth <= 0 || params.pageHeight <= 0) {
    return BookStatus::Unsupported;
  }
  const size_t marked = scratch.mark();
  LayoutEngine engine(params, scratch, sink);
  if (!engine.init()) {
    scratch.release(marked);
    return BookStatus::OutOfMemory;
  }
  BookStatus status = XmlSax::parseEntry(source, entry, scratch, engine,
                                         /*filterHtmlEntities=*/true);
  if (status == BookStatus::Ok && !engine.finish()) status = BookStatus::OutOfMemory;
  if (status == BookStatus::Ok && engine.outOfMemory()) status = BookStatus::OutOfMemory;
  if (pageCountOut != nullptr) *pageCountOut = engine.pageCount();
  scratch.release(marked);
  return status;
}

}  // namespace book
}  // namespace freeink
