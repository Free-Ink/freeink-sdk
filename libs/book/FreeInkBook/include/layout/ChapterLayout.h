#pragma once

// FreeInk SDK — streaming chapter layout for FreeInkBook (Phase 2).
//
// Turns one spine item's XHTML into paginated text without ever building a
// DOM: SAX events feed a block-flow state machine that accumulates one
// paragraph at a time, breaks lines with libunibreak (UAX #14 — including
// CJK break opportunities), and assembles pages. Memory is O(paragraph +
// page): fixed paragraph buffers plus per-page run copies that are released
// after each PageSink callback. Chapter size never changes the footprint.
//
// Phase 2 scope: block flow (p, headings, blockquote, li, br, hr), bold and
// italic runs, per-block font sizes (headings render larger — pages mix
// sizes), left alignment, greedy breaking. Justification, hyphenation,
// kerning, images, and CSS arrive in Phase 4 per the design doc.

#include <stdint.h>

#include "BookArena.h"
#include "BookFont.h"
#include "BookStorage.h"
#include "BookTypes.h"
#include "epub/ZipCatalog.h"

namespace freeink {
namespace book {

struct LayoutParams {
  int16_t pageWidth = 800;
  int16_t pageHeight = 480;
  int16_t marginLeft = 24;
  int16_t marginRight = 24;
  int16_t marginTop = 20;
  int16_t marginBottom = 20;
  uint16_t baseSizePx = 16;      // the reader's chosen body size
  const char* language = "en";   // BCP 47, for UAX #14 tailoring
  BookFont* font = nullptr;
};

// One horizontal run of same-styled text. `text` points into layout scratch
// and is valid only while the owning PageSink::onPage() call runs.
struct PageTextRun {
  const char* text;
  uint16_t len;        // bytes of UTF-8
  int16_t x;
  int16_t baselineY;
  uint16_t sizePx;     // resolved size — headings differ from body
  uint8_t styleFlags;  // StyleFlags bits
};

struct Page {
  const PageTextRun* runs;
  uint16_t runCount;
  uint32_t pageIndex;  // 0-based within the chapter
  // Chapter character offset (codepoints of extracted text) of this page's
  // first text run. Whitespace collapse and entity resolution are layout-
  // parameter independent, so this offset addresses the same place in the
  // chapter at any font size or page geometry — it is the reading-position
  // anchor that survives relayouts.
  uint32_t charStart;
};

class PageSink {
 public:
  virtual ~PageSink() = default;
  // Called once per completed page, in order. Return false to stop layout
  // early (e.g. only the first pages are needed). Page data must be consumed
  // or copied before returning.
  virtual bool onPage(const Page& page) = 0;
};

class ChapterLayout {
 public:
  // Streams `entry` (an XHTML content document) through layout, delivering
  // pages to `sink`. All working memory comes from `scratch` and is released
  // before returning. `pageCountOut` (optional) receives the number of pages
  // delivered.
  static BookStatus layout(BookSource& source, const ZipEntry& entry,
                           const LayoutParams& params, Arena& scratch, PageSink& sink,
                           uint32_t* pageCountOut = nullptr);
};

}  // namespace book
}  // namespace freeink
