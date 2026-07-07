// FreeInkBook — stb_truetype-backed BookFont with arena-bounded caches.

#include "render/TtfFont.h"

#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
// FreeInkBook only uses stb's direct bitmap APIs, which allocate nothing;
// rasterization goes through stbtt_MakeGlyphBitmap into arena memory.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_truetype.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace freeink {
namespace book {

namespace {

stbtt_fontinfo* fontOf(uint8_t* storage) { return reinterpret_cast<stbtt_fontinfo*>(storage); }

const stbtt_fontinfo* fontOf(const uint8_t* storage) {
  return reinterpret_cast<const stbtt_fontinfo*>(storage);
}

}  // namespace

bool TtfFont::init(const uint8_t* data, uint32_t len, Arena& glyphArena) {
  static_assert(sizeof(fontInfo_) >= sizeof(stbtt_fontinfo), "fontInfo_ storage too small");
  ready_ = false;
  if (data == nullptr || len < 12) return false;
  const int offset = stbtt_GetFontOffsetForIndex(data, 0);
  if (offset < 0) return false;
  if (stbtt_InitFont(fontOf(fontInfo_), data, offset) == 0) return false;

  int asc = 0;
  int desc = 0;
  int gap = 0;
  stbtt_GetFontVMetrics(fontOf(fontInfo_), &asc, &desc, &gap);
  unitsAscent_ = asc;
  unitsDescent_ = desc;
  unitsLineGap_ = gap;

  glyphArena_ = &glyphArena;
  advances_ = glyphArena.allocArray<AdvanceSlot>(kAdvanceSlots);
  glyphs_ = glyphArena.allocArray<GlyphSlot>(kGlyphSlots);
  if (advances_ == nullptr || glyphs_ == nullptr) return false;
  for (uint32_t i = 0; i < kAdvanceSlots; ++i) advances_[i] = AdvanceSlot{};
  for (uint32_t i = 0; i < kGlyphSlots; ++i) glyphs_[i] = GlyphSlot{};
  glyphBase_ = glyphArena.mark();

  ready_ = true;
  return true;
}

float TtfFont::scaleFor(uint16_t sizePx) const {
  return stbtt_ScaleForPixelHeight(fontOf(fontInfo_), static_cast<float>(sizePx));
}

int32_t TtfFont::glyphIndexFor(uint32_t codepoint) {
  const uint32_t slot = codepoint % kAdvanceSlots;
  if (advances_[slot].key != codepoint) {
    const int glyph = stbtt_FindGlyphIndex(fontOf(fontInfo_), static_cast<int>(codepoint));
    int adv = 0;
    int lsb = 0;
    stbtt_GetGlyphHMetrics(fontOf(fontInfo_), glyph, &adv, &lsb);
    advances_[slot] = {codepoint, glyph, adv};
  }
  return advances_[slot].glyphIndex;
}

bool TtfFont::hasGlyph(uint32_t codepoint) const {
  return ready_ &&
         stbtt_FindGlyphIndex(fontOf(fontInfo_), static_cast<int>(codepoint)) != 0;
}

int16_t TtfFont::advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) {
  (void)styleFlags;
  if (!ready_) return 0;
  glyphIndexFor(codepoint);  // fills the slot
  const AdvanceSlot& slot = advances_[codepoint % kAdvanceSlots];
  const float scaled = slot.unscaledAdvance * scaleFor(sizePx);
  return static_cast<int16_t>(scaled + 0.5f);
}

int16_t TtfFont::lineHeight(uint16_t sizePx) {
  if (!ready_) return static_cast<int16_t>(sizePx);
  const float scale = scaleFor(sizePx);
  return static_cast<int16_t>((unitsAscent_ - unitsDescent_ + unitsLineGap_) * scale + 0.5f);
}

int16_t TtfFont::ascent(uint16_t sizePx) {
  if (!ready_) return static_cast<int16_t>(sizePx);
  return static_cast<int16_t>(unitsAscent_ * scaleFor(sizePx) + 0.5f);
}

int16_t TtfFont::kerning(uint32_t left, uint32_t right, uint16_t sizePx, uint8_t styleFlags) {
  (void)styleFlags;
  if (!ready_) return 0;
  const int32_t g1 = glyphIndexFor(left);
  const AdvanceSlot leftSlot = advances_[left % kAdvanceSlots];
  const int32_t g2 = glyphIndexFor(right);
  // glyphIndexFor(right) may have evicted the left slot on collision; use
  // the copy taken above.
  if (leftSlot.key != left || g1 == 0 || g2 == 0) return 0;
  const int unscaled = stbtt_GetGlyphKernAdvance(fontOf(fontInfo_), g1, g2);
  if (unscaled == 0) return 0;
  const float scaled = unscaled * scaleFor(sizePx);
  return static_cast<int16_t>(scaled + (scaled < 0 ? -0.5f : 0.5f));
}

void TtfFont::flushGlyphs() {
  glyphArena_->release(glyphBase_);
  for (uint32_t i = 0; i < kGlyphSlots; ++i) glyphs_[i] = GlyphSlot{};
}

const GlyphBitmap* TtfFont::rasterize(uint32_t codepoint, uint16_t sizePx) {
  if (!ready_) return nullptr;
  const uint64_t key = (static_cast<uint64_t>(codepoint) << 16) | sizePx;
  GlyphSlot& slot = glyphs_[(codepoint * 31 + sizePx) % kGlyphSlots];
  if (slot.key == key) return &slot.glyph;

  const int glyph = stbtt_FindGlyphIndex(fontOf(fontInfo_), static_cast<int>(codepoint));
  if (glyph == 0) return nullptr;
  const float scale = scaleFor(sizePx);

  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  stbtt_GetGlyphBitmapBox(fontOf(fontInfo_), glyph, scale, scale, &x0, &y0, &x1, &y1);
  const int w = x1 - x0;
  const int h = y1 - y0;

  uint8_t* pixels = nullptr;
  if (w > 0 && h > 0) {
    pixels = static_cast<uint8_t*>(glyphArena_->alloc(static_cast<size_t>(w) * h, 1));
    if (pixels == nullptr) {
      flushGlyphs();  // arena full — start a fresh cache generation
      pixels = static_cast<uint8_t*>(glyphArena_->alloc(static_cast<size_t>(w) * h, 1));
      if (pixels == nullptr) return nullptr;  // single glyph bigger than arena
    }
    stbtt_MakeGlyphBitmap(fontOf(fontInfo_), pixels, w, h, w, scale, scale, glyph);
  }

  int adv = 0;
  int lsb = 0;
  stbtt_GetGlyphHMetrics(fontOf(fontInfo_), glyph, &adv, &lsb);

  slot.key = key;
  slot.glyph.pixels = pixels;
  slot.glyph.width = static_cast<uint16_t>(w > 0 ? w : 0);
  slot.glyph.height = static_cast<uint16_t>(h > 0 ? h : 0);
  slot.glyph.xoff = static_cast<int16_t>(x0);
  slot.glyph.yoff = static_cast<int16_t>(y0);
  slot.glyph.advance = static_cast<int16_t>(adv * scale + 0.5f);
  return &slot.glyph;
}

// --- FontChain -----------------------------------------------------------------

bool FontChain::add(RenderFont* font) {
  if (font == nullptr || count_ >= 4) return false;
  fonts_[count_++] = font;
  return true;
}

RenderFont* FontChain::fontFor(uint32_t codepoint) {
  for (uint8_t i = 0; i < count_; ++i) {
    if (fonts_[i]->hasGlyph(codepoint)) return fonts_[i];
  }
  return count_ > 0 ? fonts_[0] : nullptr;
}

int16_t FontChain::advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) {
  RenderFont* font = fontFor(codepoint);
  return font != nullptr ? font->advance(codepoint, sizePx, styleFlags) : 0;
}

int16_t FontChain::lineHeight(uint16_t sizePx) {
  return count_ > 0 ? fonts_[0]->lineHeight(sizePx) : static_cast<int16_t>(sizePx);
}

int16_t FontChain::ascent(uint16_t sizePx) {
  return count_ > 0 ? fonts_[0]->ascent(sizePx) : static_cast<int16_t>(sizePx);
}

int16_t FontChain::kerning(uint32_t left, uint32_t right, uint16_t sizePx,
                           uint8_t styleFlags) {
  RenderFont* a = fontFor(left);
  RenderFont* b = fontFor(right);
  if (a == nullptr || a != b) return 0;
  return a->kerning(left, right, sizePx, styleFlags);
}

}  // namespace book
}  // namespace freeink
