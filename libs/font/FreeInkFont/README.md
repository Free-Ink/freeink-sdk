# FreeInkFont

A standalone TTF/OTF font engine for e-paper firmware: real advances, kerning,
ligatures, per-codepoint fallback, and on-demand glyph rasterization to 8-bit
alpha bitmaps — **decoupled from any layout or book engine**. Depend on this
library alone to render fonts; you do not need the EPUB engine.

It is freestanding C++17 with no Arduino/ESP-IDF dependency. Font file bytes can
be borrowed from memory or read through a caller-owned stream. Consumers that
need a fixed memory budget can install process-wide allocation callbacks before
the first FreeType-backed operation.

## API

- **`freeink::font::Font`** — metrics only: `advance()`, `lineHeight()`,
  `ascent()`, `kerning()`, `ligature()`, `covers()`. A layout pass can run
  host-side against this with no font files at all.
- **`freeink::font::RasterFont : Font`** — adds `hasGlyph()` and
  `rasterize(codepoint, sizePx) → const GlyphBitmap*` (8-bit coverage +
  bearings + advance).
- **`freeink::font::TtfFont : RasterFont`** — the stb_truetype backend.
  `init(const uint8_t* data, uint32_t len, Arena& glyphArena)`; the data is
  borrowed (PSRAM / mmap / arena-loaded SD file).
- **`freeink::font::FontChain : Font`** — up to 8 faces with per-codepoint,
  per-style fallback (user → Latin → CJK) so mixed scripts don't render tofu.
- **`freeink::font::Arena`** — the bump allocator backing glyph/advance caches.
- **`freeink::font::GlyphBitmap`** — `{pixels(w*h, 8-bit), width, height, xoff,
  yoff, advance}`.
- **`freeink::font::FtFont`** — the FreeType backend plus generic low-level APIs
  for 26.6 pixel sizing and metrics, glyph-ID rendering, face inspection, and
  caller-provided allocation. The SDK deliberately does not convert points or
  assume a display DPI/PPI; that policy belongs to the application or board.

Cache sizes are tunable via `-DFREEINK_FONT_ADVANCE_SLOTS` /
`-DFREEINK_FONT_GLYPH_SLOTS` (defaults 512 / 128).

## Using it from a third-party renderer (e.g. CrossPoint)

The whole point: adopt runtime TTF **without** replacing your existing text
layout. Add this one library, then bridge your renderer's font call-sites to
`RasterFont`:

```cpp
#include <TtfFont.h>          // FreeInkFont
using namespace freeink::font;

static uint8_t glyphArenaBuf[48 * 1024];   // ~32-64 KB per active size
Arena glyphArena(glyphArenaBuf, sizeof glyphArenaBuf);

TtfFont face;
face.init(ttfBytesInPsram, ttfLen, glyphArena);   // bytes borrowed

// measure:
int w = face.advance(cp, sizePx, StyleNone);
// draw:
if (const GlyphBitmap* g = face.rasterize(cp, sizePx)) {
  blit(g->pixels, g->width, g->height, penX + g->xoff, baseline + g->yoff);
}
```

Your pagination, line-breaking, and page cache stay exactly as they are — you
swap only the font backend (e.g. from a pre-rasterized bitmap format to live
outlines).

## Backends: stb_truetype and FreeType

- **`TtfFont`** — stb_truetype. Small, no extra deps; renders a font's default
  master only (no variable-font axes).
- **`FtFont`** — FreeType (vendored under `third_party/freetype`). Reads OpenType
  **variable-font axes** (real bold from the `wght` axis, real/oblique italic),
  streams large CJK faces, and does GPOS/kerning. Use this for variable fonts,
  multi-weight families, or CJK on constrained RAM. Both implement the same
  `RasterFont` interface, so consumers pick a backend without other changes.

### FtFont hinting options

`FtFont::setRenderOptions(RenderOptions)` picks the hinting/rasterization
strategy per face: `HintingMode::{None, Auto, Light, Native}`, plus
`monochrome` (1-bit output) and `stemDarkening` (auto-hinter only). This is
opt-in at build time, gated by three flags so a consumer that only wants the
default grayscale-AA path pays nothing extra:

- `FREEINK_FONT_ENABLE_NATIVE_HINTING` — compiles in FreeType's TrueType
  bytecode interpreter (`ftoption.h`), needed for `HintingMode::Native` and
  its `interpreterVersion` (35/40). Off by default: the interpreter has deep
  stack frames that can overflow a small render-task stack. A consumer that
  enables it should size that stack accordingly.
- `FREEINK_FONT_ENABLE_AUTOHINT` — registers the `autofit` module, needed for
  `HintingMode::Auto`/`Light` and `stemDarkening` to do anything.
  `FT_LOAD_FORCE_AUTOHINT` is a no-op without it.
- `FREEINK_FONT_ENABLE_MONOCHROME` — registers the classic B/W `raster`
  (`raster1`) renderer, needed for `monochrome` output. `FT_RENDER_MODE_MONO`
  fails without it.

None of these three change `TtfFont`. For `FtFont`, the "opt-in" guarantee is
precise, not absolute: compiling `FREEINK_FONT_ENABLE_AUTOHINT` and/or
`_MONOCHROME` in, by themselves, never change a caller's output if they never
touch `RenderOptions` — `HintingMode::Default` sets `FT_LOAD_NO_AUTOHINT` so
it can never silently prefer the auto-hinter just because the module became
available (confirmed identical output, hashed across sizes/codepoints, to a
build with neither compiled — see `test/host/run.sh`). `Default` deliberately
does **not** also set `FT_LOAD_NO_HINTING`: that bit gates both the native
bytecode hinter *and* FreeType's always-on phantom-point advance rounding
together (`ttgload.c`'s `IS_HINTED`), so disabling it to block one disables
the other too, truncating instead of rounding every advance — an earlier
version of this change did exactly that and regressed pagination in the
*default, no-flags* build, which is worse than the bug it was meant to fix.
When `FREEINK_FONT_ENABLE_NATIVE_HINTING` is compiled, `Default` may use the
native hinter with FreeType's normal interpreter version 40; when it is not
compiled, the same flags retain normal hinted advance behavior without an
auto-hint fallback. `Native` is the explicit mode for selecting interpreter
v35 or v40. `setRenderOptions()` returns `false` (without refusing the request)
when it needs a module this build didn't compile in, or an `interpreterVersion`
other than 35/40, so a caller with real logging can warn instead of getting
silently degraded output.

`interpreterVersion` and `stemDarkening` are FreeType *library-wide*
properties (`FT_Property_Set` has no per-face scope), not per-`FtFont`
settings, so they are deliberately **not** applied inside `setRenderOptions()`
(a one-shot call) — that would mean whichever face's `setRenderOptions()` ran
most recently wins for every OTHER face's subsequent renders, not whichever
face is actually about to draw. Instead they're re-applied immediately before
every `FT_Load_Char` (`advance()` and `rasterize()`), from that face's own
`options_`: Native selects its requested 35/40, while every non-Native mode
restores FreeType's normal v40. Only the face genuinely rendering right now
can affect what FreeType sees, closing cross-face property clobbering in both
directions.

**Vendoring note:** the `autofit`/`raster` module sources here are from
FreeType 2.14.3, while the rest of `third_party/freetype` in this tree is
2.13.3. `find_unicode_charmap` in `ftobjs.c`/`ftobjs.h` was changed from
`static` to `FT_BASE_DEF`/`FT_BASE` to match 2.14's exported linkage, which
`afadjust.c` in `autofit` calls directly — that's the only cross-version patch
this required. Validated with a host-side ASan/UBSan build across every
`HintingMode` plus monochrome against a real bundled font
(`test/host/FtFontRenderOptionsTest.cpp`), but the base FreeType tree hasn't
been bumped to 2.14 wholesale, so other 2.13/2.14 internal drift is possible
in code paths this didn't exercise. Bumping the whole vendored tree to one
matching version is worth doing as a follow-up.

### FtFont ligatures: GSUB vs. TtfFont's compatibility-codepoint check

Neither FreeType nor stb_truetype does OpenType Layout shaping (that's
normally HarfBuzz's job — too heavy to vendor here for one feature).
`TtfFont::ligature()` works around this by checking whether the font happens
to expose the ff/fi/fl/ffi/ffl glyphs at their legacy Unicode compatibility
codepoints (U+FB00–FB04), independent of what the font's actual layout tables
say. `FtFont::ligature()` instead reads the face's real `GSUB` table
(`Gsub.h` — a minimal, bounds-checked parser for just the `liga`/`rlig`
feature, lookup types 4 and 7-wrapping-4) and resolves the same five pairs
through it.

The parser treats the FeatureList as a pool: only features referenced by the
`latn` script's DefaultLangSys are active for this API, with `DFLT`'s
DefaultLangSys as the fallback. Required and listed feature indices are honored
in deterministic order; unrelated scripts and named language systems are not
silently applied. Malformed offsets and counts return no substitution, and a
bounded total-work guard protects the parser without rejecting ordinary fonts
whose active feature appears late in a large FeatureList.

This is strictly more faithful to what the font actually specifies, but it
exposes a real boundary in the `Font::ligature()` contract: it must return a
Unicode **codepoint** (the layout pass bakes it into cached page text), while
GSUB substitutes to a **glyph ID** that commonly has no codepoint at all. For
example, CrossInk's bundled Bitter font defines a real 3-glyph GSUB ligature
for "ffi" (glyph 427), but nothing in Bitter's cmap maps any codepoint to
glyph 427 — calling `ligature(0xFB00, 'i')` directly on Bitter correctly
returns 0 even though GSUB did match, because there is no codepoint this
contract could hand back for it. (Bitter also has no 2-glyph "ff" rule at
all, so a real layout consumer following `Font.h`'s documented chaining — call
`ligature('f','f')`, only chain into `ligature(0xFB00, right)` if that
returned nonzero — would never reach this specific case for Bitter; it's real
for fonts whose "ff" pair *does* have a codepoint but "ffi" doesn't.)
`ligatureGlyphId()` is the unrestricted escape hatch: it returns 427 directly,
for a consumer with its own glyph-indexed cache (not bound by "must become
cacheable page text") to use as-is.

Both are validated in `test/host/FtFontLigatureTest.cpp` against
DejaVuSans.ttf — hard-asserted against its real, committed-fixture values
(`ligature('f','i') == 0xFB01`, chained `ligature(0xFB00,'i') == 0xFB03`,
etc.), a null-pointer safety check on `Gsub::LigatureGlyphId`, and a
cache-invalidation check against a byte-identical copy of the same font with
its GSUB table's length zeroed in the sfnt directory (`stripGsubTable()`) —
same glyph IDs, zero ligatures, built at test time instead of needing a
second binary font fixture. Run via `test/host/run_ligature.sh`.

Two things worth knowing if you extend this:

- Memory-backed `init()` faces retain a borrowed pointer to the whole sfnt and
  point `gsubTable_` at its bounds-checked GSUB table, so regular/bold/italic
  faces over one resident font do not duplicate a large GSUB allocation. A
  borrowed table is still rejected when its in-bounds declared size exceeds
  the 1 MiB parse-sanity ceiling. The font bytes must outlive every face.
  `initStream()` has no whole-font view, so it copies the GSUB table lazily;
  call `setGsubByteBudget()` before the first
  ligature query to constrain that fallible allocation (the default is 1 MiB),
  and call `releaseLigatureTable()` once the needed glyph IDs are resolved.
  Release drops a borrowed view or frees an owned table, and later queries
  return zero until reinitialization.
- The GSUB view/cache is reset at the top of `init()`/`initStream()` (not just
  `deinit()`), because this same `FtFont` instance can be rebuilt with
  completely different bytes without an intervening `deinit()` call, and
  glyph IDs are face-local — a cached table from the PREVIOUS face would
  resolve the NEW face's glyph IDs against the wrong font, either losing a
  real ligature or (rarer, worse) matching a coincidentally-reused glyph ID
  to a wrong-but-real one. This is exactly what `stripGsubTable()`'s test
  reproduces without a second fixture.
- The per-table counts in `Gsub.cpp` (feature/lookup/subtable/ligature) are
  NOT artificially capped below their natural 16-bit range — `.has()` already
  bounds every read regardless of how large they are. Script records are
  required to be sorted by tag, so selecting `latn`/`DFLT` uses bounded binary
  lookup; the remaining nested walk is bounded separately by a 4096-iteration
  work budget. An earlier version capped these at 128/1024/512, which
  correctly stopped a hostile huge count from causing pathological scan time,
  but also silently dropped ligatures on ordinary large fonts whose
  `liga`/`rlig` feature simply sits past those indices (confirmed on real,
  non-adversarial fonts). `kMaxGsubBytes` (1 MiB) is a parse-sanity ceiling for
  both borrowed views and owned stream copies, not a memory-budget decision —
  real GSUB tables have been measured
  up to ~700 KiB. The stream allocation is now explicitly constrained by the
  caller's `setGsubByteBudget()` policy rather than silently retaining up to
1 MiB per face.

### FtFont integration APIs

Renderers that already own layout and caching can use `FtFont` without adopting
an SDK-specific page model:

- `inspectMemory()` and `inspectStream()` return reusable family/style metadata
  without retaining an open face.
- `glyphId()` and `ligatureGlyphId()` expose raw glyph IDs for glyph-indexed
  caches, including GSUB targets that have no Unicode codepoint.
- `metrics26_6()`, `metricsGlyph26_6()`, `kerning26_6()`,
  `kerningGlyphs26_6()`, and `lineMetrics26_6()` preserve fractional pixel
  measurements instead of forcing integer sizes or advances.
- `rasterize26_6()` and `rasterizeGlyph26_6()` render tightly packed 8-bit
  coverage at a caller-selected 26.6 pixel size. Consumers remain free to pack
  that coverage for their own framebuffer format.
- `RenderOptions::embolden26_6` and `RenderOptions::slant16_16` provide generic
  outline transforms in FreeType-native units alongside the hinting controls.
- `configureMemory()` installs complete allocate/free/reallocate callbacks before
  the shared FreeType library starts. This lets embedded consumers route SDK
  work through a bounded arena without the SDK depending on a particular RTOS,
  PSRAM implementation, or firmware allocator.

For example, converting a user-facing point size to pixels remains consumer
policy: `pixelSize26_6 = points * panelPpi * 64 / 72`. A 10-point request is
therefore different on 219-, 235-, and 259-PPI displays, while the SDK API stays
portable to unrelated devices and density policies.

### FreeType attribution (FTL)

`third_party/freetype` is a curated build of **FreeType** (https://freetype.org),
used under the **FreeType License (FTL)** — see `third_party/freetype/FTL.TXT`.
Per the FTL, products that include this library must credit FreeType in their
documentation:

> Portions of this software are copyright © The FreeType Project
> (www.freetype.org). All rights reserved.

## CJK / large fonts

stb_truetype needs the whole font file in RAM, which a no-PSRAM device can't do
for a multi-megabyte CJK face. A **FreeType** backend (streaming table access) is
the upgrade path and slots in behind the same `RasterFont` interface as a second
implementation alongside `TtfFont` — no API change for consumers. This library
ships the stb_truetype backend first.

## Relationship to FreeInkBook

This engine used to live inside FreeInkBook. It was extracted here unchanged;
FreeInkBook now re-exports the types under their historical names
(`freeink::book::BookFont == Font`, `RenderFont == RasterFont`, `TtfFont`,
`FontChain`, `Arena`) via thin alias headers, so its layout code and its full
host-test suite are unaffected.
