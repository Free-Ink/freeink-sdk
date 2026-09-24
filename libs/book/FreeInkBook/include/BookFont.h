#pragma once

// FreeInk SDK — font metrics / rasterization interface.
//
// The interfaces now live in the standalone FreeInkFont library
// (freeink::font::Font / RasterFont / GlyphBitmap / StyleFlags) so any consumer
// can render real fonts without pulling in the book engine. This header
// re-exports them under the historical freeink::book names — BookFont ==
// freeink::font::Font, RenderFont == freeink::font::RasterFont — so existing
// FreeInkBook layout/render code keeps compiling unchanged.

#include <Font.h>

namespace freeink {
namespace book {

using freeink::font::GlyphBitmap;

using freeink::font::StyleFlags;
using freeink::font::StyleNone;
using freeink::font::StyleBold;
using freeink::font::StyleItalic;
using freeink::font::StyleUnderline;
using freeink::font::StyleSuperscript;
using freeink::font::StyleSubscript;

using BookFont = freeink::font::Font;
using RenderFont = freeink::font::RasterFont;

}  // namespace book
}  // namespace freeink
