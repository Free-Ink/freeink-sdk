#pragma once

// FreeInk SDK — Liang-pattern hyphenation for FreeInkBook.
//
// The matcher runs over FIBH pattern data compiled offline by tools/hyphc.py
// from TeX hyph-utf8 pattern files (the same data KOReader and LibreOffice
// use, permissively licensed per language). The data is borrowed, not copied
// — point it at flash, PSRAM, or an arena-loaded file — and matching itself
// allocates nothing: it walks a sorted pattern table with an incremental
// binary search per character.
//
// Hyphenation points are offered to the line breaker as soft breaks, weighted
// below real break opportunities; the hyphen glyph is added at render time
// and never enters the chapter text, so position anchors are unaffected.

#include <stdint.h>

namespace freeink {
namespace book {

class Hyphenator {
 public:
  // Validates and adopts FIBH data (borrowed; must outlive the Hyphenator).
  bool init(const uint8_t* data, uint32_t len);
  bool ready() const { return patternCount_ > 0; }

  // Finds allowed break positions inside `word` (a run of letter bytes,
  // ASCII case-insensitive). Positions are byte offsets into `word` where a
  // line may break (hyphen rendered before the break). Respects the en-US
  // typesetting minimums: 2 letters before the first break, 3 after the
  // last. Returns the number of positions written to `out` (at most
  // `outCap`), in increasing order.
  uint8_t breakPositions(const char* word, uint32_t wordLen, uint8_t* out,
                         uint8_t outCap) const;

 private:
  const uint8_t* keyAt(uint32_t index, uint8_t* keyLenOut) const;

  const uint8_t* data_ = nullptr;
  const uint8_t* blob_ = nullptr;
  const uint32_t* offsets_ = nullptr;  // little-endian in-place
  uint32_t patternCount_ = 0;
  uint8_t maxPatLen_ = 0;

  static constexpr uint32_t kMaxWord = 64;  // bytes — Cyrillic runs 2 bytes/char
  static constexpr uint8_t kLeftMin = 2;
  static constexpr uint8_t kRightMin = 3;
};

}  // namespace book
}  // namespace freeink
