#pragma once

// FreeInk SDK — minimal OpenType GSUB ligature-substitution reader.
//
// FreeType (and stb_truetype) rasterize outlines; neither parses OpenType
// Layout (GSUB/GPOS) — that's normally HarfBuzz's job, which is too heavy to
// vendor here for one feature. This reads exactly the 'liga'/'rlig' feature
// of a GSUB table (lookup types 4, and 7 wrapping 4) for 2-3 input glyphs,
// bounds-checked and capped against malformed/hostile font data.
// Only features referenced by the `latn` script's DefaultLangSys are active;
// `DFLT`'s DefaultLangSys is the fallback. Named language systems and
// unrelated scripts are not selected because this API has no language input.
//
// Returns a GLYPH ID, not a Unicode codepoint: most professionally-authored
// OpenType fonts substitute ligatures to a glyph with NO cmap entry at all,
// so there is often no codepoint to hand back. A consumer with its own
// glyph-indexed page/glyph-cache pipeline (not tied to re-resolving codepoints
// later) can use this result directly. `Font::ligature()` (see Font.h) can't:
// its contract requires a real codepoint the layout pass can bake into cached
// page text, so FtFont::ligature() only uses this to catch the narrower case
// where the substituted glyph also happens to have a Unicode mapping — see
// its implementation for that bridge.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace font {
namespace gsub {

// `sequence` holds `length` (2 or 3) glyph IDs. Returns the substituted
// ligature glyph ID, or 0 when the table has no matching ligature (or is
// absent/malformed). `gsubBytes`/`gsubSize` is the raw content of the font's
// 'GSUB' table (e.g. from FT_Load_Sfnt_Table), not the whole font file.
uint32_t LigatureGlyphId(const uint8_t* gsubBytes, size_t gsubSize, const uint32_t* sequence, unsigned length);

}  // namespace gsub
}  // namespace font
}  // namespace freeink
