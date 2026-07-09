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
#include "css/Css.h"
#include "epub/XmlSax.h"
#include "epub/ZipCatalog.h"
#include "text/Hyphenator.h"

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

  // Typography (Phase 4).
  uint16_t lineSpacingPct = 100;                 // line height multiplier (CrossPoint parity)
  uint16_t paragraphSpacingPct = 100;            // scales block margins (0 = compact)
  TextAlign defaultAlign = TextAlign::Left;      // body alignment when CSS is silent
  uint8_t orphanLines = 2;                       // min paragraph lines at a page bottom
  uint8_t widowLines = 2;                        // min paragraph lines carried over
  bool embeddedStyles = true;                    // honor chapter <style> blocks
  bool focusReading = false;                     // bold each word's first ~45% (fixation aid)
  const CssStylesheet* stylesheet = nullptr;     // book CSS (optional)
  const Hyphenator* hyphenator = nullptr;        // soft hyphenation (optional)
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

// One placed image. `href` is the container path of the image entry,
// arena-owned with the same lifetime as the page's text.
struct PageImage {
  const char* href;
  int16_t x;
  int16_t y;        // top edge
  uint16_t width;   // placement size (aspect-preserving, never upscaled)
  uint16_t height;
};

// A tappable link region (an <a href> on this page). `target` is the
// resolved container path ("" = same chapter); `fragment` the anchor id.
struct PageLink {
  const char* target;
  const char* fragment;
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
};

struct Page {
  const PageTextRun* runs;
  uint16_t runCount;
  const PageImage* images;
  uint16_t imageCount;
  const PageLink* links;
  uint16_t linkCount;
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
  // Called for every id="" anchor as layout passes it (chapter character
  // offset) — the substrate for footnote/internal-link jumps.
  virtual void onAnchor(uint32_t idHash, uint32_t charStart) {
    (void)idHash;
    (void)charStart;
  }
  // Called once per completed page, in order. Return false to stop layout
  // early (e.g. only the first pages are needed). Page data must be consumed
  // or copied before returning.
  virtual bool onPage(const Page& page) = 0;
};

class ChapterLayout {
 public:
  // Streams `entry` (an XHTML content document) through layout, delivering
  // pages to `sink`. `zip` and `chapterHref` (the entry's own container
  // path) let img elements resolve and probe their targets. All working
  // memory comes from `scratch` and is released before returning.
  // `pageCountOut` (optional) receives the number of pages delivered.
  //
  // `parseScratch` (optional) splits the working set into two arenas: the
  // parse-side allocations (inflate window + decompressor + XML chunks —
  // ~50 KB for a deflated entry) come from it instead of `scratch`, so
  // neither arena needs to be a single ~100 KB block. On fragmented
  // PSRAM-less heaps two ~50 KB blocks fit where one large one cannot.
  // When null, everything comes from `scratch` (the classic behavior).
  static BookStatus layout(BookSource& source, const ZipCatalog& zip, const ZipEntry& entry,
                           const char* chapterHref, const LayoutParams& params, Arena& scratch,
                           PageSink& sink, uint32_t* pageCountOut = nullptr,
                           uint32_t* totalCharsOut = nullptr, Arena* parseScratch = nullptr);

  // Plain-text (.txt) layout: the whole file is one chapter, paragraphs
  // split on blank lines, single newlines flow as spaces. Justification,
  // hyphenation, caching, and character anchors all apply identically —
  // feed the same PageCacheWriter/Reader as an EPUB chapter (spine 0).
  // UTF-8 assumed; a leading BOM is skipped.
  static BookStatus layoutPlainText(BookSource& source, const LayoutParams& params,
                                    Arena& scratch, PageSink& sink,
                                    uint32_t* pageCountOut = nullptr,
                                    uint32_t* totalCharsOut = nullptr);
};

// Resumable chapter layout — the incremental-build primitive. Where
// ChapterLayout::layout() runs to completion in one call, a session lays out
// a few pages at a time so a giant single-spine chapter can show its first
// page immediately and finish behind the reader:
//
//   ChapterLayoutSession s;
//   s.begin(bookSource, &zip, chapterSource, entry, href, params, scratch,
//           sink, &parseArena);
//   while (!s.done()) { s.step(4); /* render, handle input, ... */ }
//
// `chapterSource` may differ from `bookSource`: extract a deflated chapter to
// plain storage once and hand it in as a stored entry — the resident parse
// state then shrinks from ~46 KB (inflate window + decompressor) to ~8 KB,
// which is what makes holding a session open WHILE rendering pages viable on
// PSRAM-less hosts. Image probes always use `bookSource` + `zip`.
//
// Both arenas must stay valid (and must not be reset) for the session's
// lifetime. The session does not release its arena marks — the owner resets
// the arenas after abort()/completion.
class ChapterLayoutSession {
 public:
  ChapterLayoutSession() = default;
  ~ChapterLayoutSession() { abort(); }
  ChapterLayoutSession(const ChapterLayoutSession&) = delete;
  ChapterLayoutSession& operator=(const ChapterLayoutSession&) = delete;

  // Runs the image pre-scan (cheap on a stored chapterSource) and opens the
  // parse. `zip` may be null for containerless input.
  //
  // `prescanScratch` (optional): the pre-scan probes image targets in the
  // BOOK container, which needs a transient inflate stream (~46 KB) even
  // when the chapter itself is stored. A resident session should pass a
  // TEMPORARY large arena here (freed right after begin() returns) and keep
  // `parseScratch` at the small stored-entry size (~12 KB) — otherwise the
  // parse arena must be sized for the probe peak for its whole lifetime.
  BookStatus begin(BookSource& bookSource, const ZipCatalog* zip, BookSource& chapterSource,
                   const ZipEntry& entry, const char* chapterHref, const LayoutParams& params,
                   Arena& scratch, PageSink& sink, Arena* parseScratch = nullptr,
                   Arena* prescanScratch = nullptr);

  // Feeds the parse until at least `minNewPages` more pages were delivered to
  // the sink or the chapter ends (granularity is one input chunk, so it can
  // overshoot by a page). On the final step the layout tail is flushed and
  // done() becomes true. Returns Ok on progress, or the failure status.
  BookStatus step(uint32_t minNewPages);

  bool done() const { return state_ == State::Done; }
  bool active() const { return state_ == State::Parsing; }
  uint32_t pagesEmitted() const;
  // Valid once done(): total extracted characters (progress denominator).
  uint32_t totalChars() const;
  // Input-side progress for estimated-total-pages: bytes fed / entry size.
  uint64_t bytesConsumed() const { return sax_.bytesConsumed(); }
  uint64_t bytesTotal() const { return bytesTotal_; }

  // Tears down the parse (safe in any state). The owner resets the arenas.
  void abort();

 private:
  enum class State : uint8_t { Idle, Parsing, Done, Failed };

  class CountingSink;  // forwards to the caller sink, counting pages

  XmlSaxSession sax_;
  void* engine_ = nullptr;        // LayoutEngine, placement-new'd in the arena
  void* countingSink_ = nullptr;  // CountingSink, placement-new'd in the arena
  uint64_t bytesTotal_ = 0;
  State state_ = State::Idle;
};

}  // namespace book
}  // namespace freeink
