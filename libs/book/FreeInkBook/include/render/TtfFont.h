#pragma once

// FreeInk SDK — TTF/OTF font engine.
//
// The engine now lives in the standalone FreeInkFont library
// (freeink::font::TtfFont / FontChain, over stb_truetype). This header maps the
// book memory profile onto the font library's cache-size knobs so the SMALL /
// DEFAULT / LARGE tiers keep their original slot counts, then re-exports the
// engine under the historical freeink::book names for existing FreeInkBook code.

#include "BookProfile.h"

// Preserve the per-profile glyph/advance cache sizing the engine used when it
// lived in FreeInkBook (see the old TtfFont.h). Define before including the
// font library header so its #ifndef guards pick these up.
#if FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_SMALL
#define FREEINK_FONT_ADVANCE_SLOTS 256
#define FREEINK_FONT_GLYPH_SLOTS 64
#elif FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_LARGE
#define FREEINK_FONT_ADVANCE_SLOTS 2048
#define FREEINK_FONT_GLYPH_SLOTS 512
#endif

#include <TtfFont.h>

namespace freeink {
namespace book {
using freeink::font::TtfFont;
using freeink::font::FontChain;
}  // namespace book
}  // namespace freeink
