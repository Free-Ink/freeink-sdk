#pragma once

// FreeInk SDK — CSS subset for FreeInkBook (Phase 4).
//
// Implements the slice of CSS that reflowable books actually use, resolved
// per element during the streaming parse — there is no style tree. The
// tokenizer is tolerant by design: unknown properties, selectors, at-rules,
// and units are skipped, never fatal.
//
// Supported selectors: element, .class, element.class (the last simple
// selector of a descendant chain is matched; combinators are treated as
// "match the rightmost"). Specificity: element < class < element.class <
// inline style.
//
// Supported properties: font-size (em/%/px/pt/rem + keywords), font-weight,
// font-style, text-align, text-indent (em/px), margin-left/margin-top/
// margin-bottom (em/px, also via the margin shorthand), display:none.

#include <stdint.h>

#include "BookArena.h"
#include "BookStorage.h"
#include "BookTypes.h"
#include "epub/ZipCatalog.h"

namespace freeink {
namespace book {

enum class TextAlign : uint8_t { Inherit = 0, Left, Right, Center, Justify };

// One declaration block with per-property "set" sentinels so later rules
// override only what they declare.
struct CssDecl {
  uint16_t sizePct = 0;        // 0 = unset; % of the parent font size
  bool sizeRootRelative = false;  // true = % of reader root size, for rem
  int8_t weightBold = -1;      // -1 unset, 0 normal, 1 bold
  int8_t styleItalic = -1;     // -1 unset, 0 normal, 1 italic
  TextAlign align = TextAlign::Inherit;
  int16_t textIndentPct = -1;  // -1 unset; % of em
  int16_t marginLeftPct = -1;   // -1 unset; % of em, applied as inline/block start margin
  int16_t marginTopPct = -1;   // -1 unset; % of em
  int16_t marginBottomPct = -1;
  int8_t displayNone = -1;     // -1 unset, 1 = display:none
  int8_t underline = -1;       // -1 unset, 0 none, 1 underline
  int8_t vertAlign = -1;       // -1 unset, 0 baseline, 1 super, 2 sub

  // Applies every property `over` declares on top of this one.
  void applyOver(const CssDecl& over);
};

struct CssRule {
  uint32_t elemHash;   // 0 = any element
  uint32_t classHash;  // 0 = no class requirement
  CssDecl decl;
};

struct CssStylesheet {
  const CssRule* rules = nullptr;
  uint16_t ruleCount = 0;
  uint32_t contentHash = 0;  // folds into the layout generation hash
};

// Accumulates rules from one or more CSS files into a single stylesheet.
// Rules and the builder's working copy live in `arena`.
class CssStylesheetBuilder {
 public:
  bool begin(Arena& arena);

  // Parses one CSS entry from the container. Sheets larger than the internal
  // read cap are skipped (counted, not fatal). Scratch is released on return.
  BookStatus addSheet(BookSource& source, const ZipEntry& entry, Arena& scratch);

  // Parses raw CSS text (for tests or <style> blocks).
  void addText(const char* css, uint32_t len);

  CssStylesheet finish();

  uint16_t skippedSheets() const { return skippedSheets_; }

 private:
  static constexpr uint16_t kMaxRules = 512;

  Arena* arena_ = nullptr;
  CssRule* rules_ = nullptr;
  uint16_t ruleCount_ = 0;
  uint16_t skippedSheets_ = 0;
  uint32_t contentHash_ = 2166136261u;
};

// Parses an inline style="" attribute value.
CssDecl parseInlineStyle(const char* text);

// Resolves the cascade for one element: rules matching (element, any of the
// element's classes) are applied in specificity then document order, then
// `inline` last. `classAttr` is the raw class attribute (may be nullptr).
CssDecl cascadeFor(const CssStylesheet& sheet, const char* elementLocalName,
                   const char* classAttr, const CssDecl* inlineDecl);

}  // namespace book
}  // namespace freeink
