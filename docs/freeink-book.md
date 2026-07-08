# FreeInkBook

FreeInkBook (`libs/book/FreeInkBook`) turns an EPUB file on external storage
into typeset, cached, tappable pages on an e-paper panel. It is a complete
reading engine — container parsing, CSS, layout, pagination, fonts, images,
links — built as a freestanding C++17 library: no Arduino or ESP-IDF
dependency in the core, every byte of working memory supplied by the caller,
and the entire pipeline runs (and is regression-tested) on a desktop host.

The design rationale, phase history, and prior-art notes live in
[freeink-book-design.md](freeink-book-design.md). This document is the
user guide.

## The four rules

Everything in the engine follows from four rules, each enforced by host
tests rather than convention:

1. **Never a DOM.** Chapters parse as a stream of SAX events feeding a
   layout state machine. RAM is O(paragraph + page) — layout peak memory is
   byte-identical between a 1 KB chapter and a 19,000-page omnibus.
2. **Arenas only.** All memory comes from caller-sized bump allocators that
   reset wholesale at book/chapter/page boundaries. There is no `free()`, so
   there is no fragmentation; exhaustion returns a status, never aborts.
3. **Layout once, render many.** Pagination runs once per (book, settings)
   generation and serializes compact page records. A page turn is one small
   read + a glyph blit — no ZIP, no XML, no layout, ~2 KB of scratch.
4. **Host-testable end to end.** `test/host/run.sh` builds four test
   binaries covering container, layout, cache, and fonts — including
   CI-asserted memory ceilings and O(1) proofs.

## Quick start

```cpp
#include <FreeInkBook.h>
#include <cache/PageCache.h>
#include <layout/ChapterLayout.h>
#include <render/PageRenderer.h>
#include <render/TtfFont.h>

using namespace freeink::book;

// 1. Adapters: the engine never touches a filesystem directly.
MyBookSource source;      // BookSource: readAt()/size() over the .epub
MyCacheStorage cache;     // CacheStorage: read/streaming-write cache files

// 2. Arenas: the caller decides every budget (PSRAM on ESP32-class parts).
Arena bookArena(bookBuf, 512 * 1024);   // retained while the book is open
Arena scratch(scratchBuf, 512 * 1024);  // transient; released per call

// 3. Open: ZIP catalog, OPF, TOC — metadata/spine/toc are now queryable.
Book book;
if (book.open(source, bookArena, scratch) != BookStatus::Ok) ...;

// 4. Fonts: any TTF/OTF, style-aware, per-codepoint fallback.
TtfFont serif;  serif.init(ttfBytes, ttfLen, glyphArena);
FontChain fonts; fonts.add(&serif);            // + bold/italic faces, CJK...

// 5. Layout params = every reader setting in one struct.
LayoutParams params;
params.pageWidth = 480;  params.pageHeight = 800;   // logical (portrait)
params.baseSizePx = 18;  params.font = &fonts;
params.defaultAlign = TextAlign::Justify;
params.hyphenator = &hyphenator;                    // see Hyphenation

// 6. Paginate into the cache (once per generation), then read pages forever.
char name[64];
const uint32_t gen = layoutGenerationHash(params, fontFingerprint);
pageCacheName(spineIndex, gen, name, sizeof(name));
PageCacheReader reader;
if (reader.open(cache, name, gen, indexArena) != BookStatus::Ok) {
  PageCacheWriter writer;
  writer.begin(cache, name, gen, scratch);
  uint32_t totalChars = 0;
  ChapterLayout::layout(source, book.zip(), *entry, item->href, params,
                        scratch, writer, nullptr, &totalChars);
  writer.setTotalChars(totalChars);
  writer.finish();
  reader.open(cache, name, gen, indexArena);
}

// 7. Render a page into a 1-bit (or Gray8) framebuffer.
Page page{};
reader.readPage(pageIndex, scratch, &page);
FrameTarget frame{fb, panelW, panelH, panelWBytes,
                  FrameFormat::Mono1Dithered, FrameRotation::Portrait};
PageRenderer::render(page, fonts, source, book.zip(), scratch, frame);
```

A complete firmware consuming all of this (screens, settings, progress,
gestures) is a few hundred lines; the storage adapters are ~80.

## Module map

| Area | Headers | What it does |
|---|---|---|
| Container | `FreeInkBook.h`, `epub/ZipCatalog.h` | ZIP catalog + streaming inflate; OPF metadata/manifest/spine; nav + NCX TOC; DRM detection |
| CSS | `css/Css.h` | Tolerant subset cascade: element/`.class` selectors, ~15 properties, inline `style=""`, chapter `<style>` blocks, book-level builder |
| Layout | `layout/ChapterLayout.h` | SAX → block flow → UAX #14 line breaking → two-phase paragraph placement → `PageSink`; `layoutPlainText()` drives the same engine for .txt files |
| Cache | `cache/PageCache.h` | FIBP page records: generation hash, torn-write detection, char anchors, id-anchor table, per-chapter totals |
| Fonts | `BookFont.h`, `render/TtfFont.h` | `RenderFont` interface; stb_truetype engine (kerning, ligatures, AA); style-aware `FontChain` |
| Render | `render/PageRenderer.h`, `render/ImageRenderer.h` | Page → framebuffer compositor (mono dithered/sharp/Gray8, 4 rotations); streaming image decode with box-filter + Floyd–Steinberg |
| Text | `text/Hyphenator.h`, `text/EntityFilter.h` | Liang pattern hyphenation; HTML named-entity repair for strict XML |
| Tools | `tools/hyphc.py`, `tools/arabshapec.py`, `tools/fibcheck.cpp` | Hyphenation pattern compiler (SD blob or embeddable header); Arabic shaping table generator (from vendored UCD extracts); whole-book pipeline checker for corpus testing |

## Typography

- **Line breaking** is full Unicode UAX #14 (vendored libunibreak): correct
  break opportunities for every script, including CJK. Kinsoku is the
  spec-defined behavior — pass `language = "ja"` for normal or `"ja-strict"`
  to also forbid small kana and `ー` at line starts. Korean breaks between
  syllables by default (the UAX #14 behavior); pass `"ko-keep-all"` for
  word-unit breaking (CSS `word-break: keep-all` style).
- **Justification** distributes spare space across word gaps (Latin) or
  inter-character gaps (spaceless CJK); mixed lines pool both. Korean —
  the spaced CJK script — stretches its word spaces like English and only
  falls back to inter-syllable expansion on space-less lines, so justified
  words never visibly tear apart. Justified lines end exactly at the
  margin; last lines and hard breaks never stretch.
- **Full-width punctuation compression** (JLREQ pair rule): adjacent
  full-width punctuation (。」 」（ 『「 …) compresses by half an em, so
  Japanese/Chinese quote-and-stop clusters set tight instead of gapping a
  full em. The quarter-em CJK↔Latin gap applies to Japanese/Chinese
  contexts only — Korean attaches particles directly to Latin words
  ("TV를") with no added air.
- **Hyphenation** keeps justified gaps tight. `tools/hyphc.py` compiles any
  TeX hyph-utf8 pattern file into either a loadable blob or an embeddable
  header (`text/hyph_en_us.h` ships pre-generated, ~71 KB of flash). The
  matcher is UTF-8 aware — Cyrillic, Greek, and accented-Latin words
  hyphenate, with case folding and character-based minimums. Soft hyphens
  (U+00AD) are zero-width and render a hyphen only when a line actually
  breaks there.
- **Styles**: per-run font sizes (headings, `small`, `sub`/`sup` with real
  baseline shifts), bold/italic via real faces or synthetic double-strike,
  underline, kerning-aware measurement, and **ligatures** (fi fl ff ffi ffl
  substitute when the face has the glyphs; baked into page records so
  rendering needs no shaping logic).
- **Paragraphs**: widow/orphan control, first-line `text-indent`, block
  margins, `lineSpacingPct` / `paragraphSpacingPct` knobs.
- **Focus reading** (`LayoutParams.focusReading`): bolds each word's first
  ~45% of characters (clamped 1–9) as a fixation aid — resolved at layout
  time into ordinary bold runs, so any bold face (or synthetic bold)
  renders it.
- **Bidi (RTL)** is built in: paragraphs are classified per UAX #9
  (first-strong detection, embedding levels, run reordering, bracket
  mirroring), so Hebrew and Arabic text — including embedded Latin words and
  numbers — lays out and renders correctly with no renderer involvement:
  page records store glyphs in visual order. RTL paragraphs mirror the
  alignment (left becomes right) and skip first-line indent.
- **Arabic shaping**: letters take their contextual joining forms (isolated/
  initial/medial/final per Unicode ch. 9, transparent marks skipped, the
  mandatory lam-alef ligature fused) as Presentation Forms codepoints baked
  into page records — resolved once per paragraph from UCD-generated tables
  (`tools/arabshapec.py`), correct in both measurement and rendering. The
  font must cover the Presentation Forms blocks (U+FB50–FEFF; most classic
  Arabic faces do — fonts that only shape via OpenType GSUB fall back to
  base letters, guarded per glyph by `BookFont::covers()`).

## Position, links, progress

Every page record carries `charStart` — the codepoint offset of its first
run within the chapter's extracted text. Because that offset is independent
of every layout parameter, it is the universal locator:

- **Progress / bookmarks** are `(spineIndex, charStart)` pairs; restore with
  `PageCacheReader::pageForChar()`. Changing font, size, margins, spacing,
  alignment, or orientation relayouts into a new generation and lands on the
  same sentence.
- **Links**: `<a href>` regions become `PageLink` rects in page records;
  `id=""` anchors are recorded (hash → charStart) in the cache, so a tapped
  footnote resolves via `charForAnchor()` → `pageForChar()` — exactly.
- **Reading percentage**: each chapter cache stores its total character
  count (`PageCacheReader::totalChars()`), the denominator sync protocols
  (e.g. KOReader-style) need.

## Fonts

`RenderFont` is the one interface (metrics + `rasterize()` + `hasGlyph()` +
`ligature()`). Implementations:

- **`TtfFont`** — stb_truetype over a borrowed pointer: SD-loaded into
  PSRAM, memory-mapped from a flash partition, or compiled in. Real
  'kern'/GPOS kerning, true 8-bit coverage, arena-bounded glyph cache with
  flush-and-rebuild. The pointer contract is the point: any addressable
  bytes work, so PSRAM-less MCUs can serve TTFs from mapped flash.
- **`FontChain`** — up to 8 faces registered with style flags
  (regular/bold/italic/bold-italic); per-codepoint fallback so mixed scripts
  never render tofu; cross-face ligatures and kerning are refused so widths
  never lie.
- **FreeInkUI bridges** (opt-in headers in the UI library):
  `BitmapBookFont` reads books with the bundled bitmap font — zero font
  files required — and `TtfGlyphSource` feeds UI chrome missing-glyph
  fallback (Hangul/CJK titles) from a TtfFont.

## Images

`<img>` targets are probed for dimensions at layout time (header-only, no
decode), placed aspect-fit and centered, and recorded in the page. At render
time `ImageRenderer` streams the decode — PNG scanline-by-scanline, JPEG in
MCU bands — through box-filter resampling into Floyd–Steinberg dithering
(1-bit) or raw grayscale (Gray8). The full image never exists in memory.
Unsupported formats (GIF, SVG, interlaced PNG) leave their reserved space
blank rather than failing the page.

## Memory profiles

One build flag (`BookProfile.h`) tiers every fixed working set to the
target's RAM class — CI builds and runs the layout suite under all three:

| Profile | Flag | Fixture peak | For |
|---|---|---|---|
| Small | `-DFREEINK_BOOK_SMALL=1` | ~106 KB | PSRAM-less MCUs (ESP32-C3) |
| Standard | *(default)* | ~152 KB | PSRAM parts (ESP32-S3) |
| Large | `-DFREEINK_BOOK_LARGE=1` | ~231 KB | big PSRAM budgets, host tools |

Small shrinks the fixed buffers gracefully: very long paragraphs flush in
segments, dense pages split a line early, fewer links/images per page —
measured on real books, text chapters peak ~62 KB and image chapters
~153 KB (two inflate states live during an image probe — the known
remaining squeeze). Large buys headroom for pathological books and much
bigger font caches (fewer glyph re-rasterizations → faster page renders).
Runtime capabilities — TTF vs bitmap fonts, Gray8 vs 1-bit rendering,
arena budgets — are orthogonal and chosen by the caller per device; the
profile only sets the compile-time ceilings around them.

Book arenas: 64 KB covers normal books (corpus high-water 5–40 KB); webnovel
omnibuses with 1,800 ZIP entries need ~450 KB. Exhaustion is a clean
`OutOfMemory` status.

## Cache format and invalidation

Cache files are versioned (`FIBP` v3) and keyed by
`layoutGenerationHash()` — a hash of every layout-relevant input: page
geometry, margins, base size, spacing, alignment, orphan/widow, stylesheet
content, hyphenator presence, language, the caller's font fingerprint, plus
an internal `kLayoutRevision` bumped whenever layout *behavior* changes
without a format change. Wrong hash → `Stale` → rebuild; torn writes are
detected by a footer magic. Callers should fingerprint the actual font set
(name + style coverage), not just "a font was present."

## Validation

Beyond the host suites, `tools/fibcheck.cpp` runs any real EPUB through the
whole pipeline and reports per-chapter status, page counts, font sizes seen,
and arena high-water marks. The current corpus: ~50 commercial and
pathological books across Latin, Cyrillic, and CJK — including Ulysses, a
3,742-page omnibus, and 1,700-chapter webnovels — all clean, with DRM'd
books correctly rejected as `Encrypted`.

## Known limitations

Dropcap float wrap-around (caps render at size, text does not wrap beside
them), Arabic vowel-mark positioning (harakat shape-correctly but render as
spacing glyphs, and a mark between lam and alef defeats that ligature),
Arabic shaping with GSUB-only fonts (Presentation Forms coverage required),
interlaced PNG, per-run character offsets for highlight ranges, and
streaming (larger than addressable memory) fonts — FreeType behind the same
`BookFont` interface is the documented path.
