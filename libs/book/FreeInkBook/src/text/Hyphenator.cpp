// FreeInkBook — Liang pattern matcher over FIBH data.

#include "text/Hyphenator.h"

#include <string.h>

namespace freeink {
namespace book {

namespace {

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Case folding for the scripts hyphenation patterns cover (Latin incl.
// accents, Greek, Cyrillic). Every mapping here preserves UTF-8 byte length,
// so folding in place keeps all byte offsets stable.
uint32_t foldCase(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
  if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) return cp + 0x20;  // Latin-1
  // Latin Extended-A: upper/lower pairs (with the three parity flips).
  if (cp >= 0x0100 && cp <= 0x0137 && (cp & 1) == 0) return cp + 1;
  if (cp >= 0x0139 && cp <= 0x0148 && (cp & 1) == 1) return cp + 1;
  if (cp >= 0x014A && cp <= 0x0177 && (cp & 1) == 0) return cp + 1;
  if (cp == 0x0178) return 0x00FF;  // Ÿ
  if (cp >= 0x0179 && cp <= 0x017E && (cp & 1) == 1) return cp + 1;
  if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) return cp + 0x20;  // Greek
  if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;                  // Cyrillic
  if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;                  // Ё etc.
  return cp;
}

uint32_t decodeUtf8At(const uint8_t* s, uint32_t len, uint32_t& i) {
  const uint8_t b = s[i];
  uint32_t cp = b;
  uint32_t extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
  if (extra == 1) cp = b & 0x1F;
  if (extra == 2) cp = b & 0x0F;
  if (extra == 3) cp = b & 0x07;
  if (i + 1 + extra > len) extra = len - i - 1;  // clamp a truncated tail
  for (uint32_t k = 0; k < extra; ++k) {
    cp = (cp << 6) | (s[i + 1 + k] & 0x3F);
  }
  i += 1 + extra;
  return cp;
}

uint32_t encodeUtf8To(uint32_t cp, uint8_t* out) {
  if (cp < 0x80) {
    out[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    out[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
  out[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
  out[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  return 3;
}

}  // namespace

bool Hyphenator::init(const uint8_t* data, uint32_t len) {
  patternCount_ = 0;
  if (data == nullptr || len < 12 || memcmp(data, "FIBH", 4) != 0) return false;
  const uint16_t version = static_cast<uint16_t>(data[4] | (data[5] << 8));
  if (version != 1) return false;
  maxPatLen_ = data[6];
  const uint32_t count = getU32(data + 8);
  if (len < 12 + count * 4) return false;
  data_ = data;
  offsets_ = reinterpret_cast<const uint32_t*>(data + 12);
  blob_ = data + 12 + count * 4;
  patternCount_ = count;
  return true;
}

const uint8_t* Hyphenator::keyAt(uint32_t index, uint8_t* keyLenOut) const {
  const uint8_t* rec = blob_ + getU32(reinterpret_cast<const uint8_t*>(offsets_ + index));
  *keyLenOut = rec[0];
  return rec + 1;
}

uint8_t Hyphenator::breakPositions(const char* word, uint32_t wordLen, uint8_t* out,
                                   uint8_t outCap) const {
  if (!ready() || wordLen < kLeftMin + kRightMin || wordLen > kMaxWord - 2) return 0;

  // Boundary-marked case-folded copy: ".word.". Folding is codepoint-wise
  // but byte-length preserving, so offsets into `marked` match `word`.
  // charIndex[byte] = 0-based character ordinal (patterns' typesetting
  // minimums count characters, not bytes).
  uint8_t marked[kMaxWord + 2];
  uint8_t charIndex[kMaxWord + 2];
  marked[0] = '.';
  uint32_t charCount = 0;
  {
    const uint8_t* src = reinterpret_cast<const uint8_t*>(word);
    uint32_t i = 0;
    while (i < wordLen) {
      const uint32_t at = i;
      const uint32_t cp = decodeUtf8At(src, wordLen, i);
      const uint32_t n = encodeUtf8To(foldCase(cp), marked + 1 + at);
      if (at + n > wordLen) return 0;  // truncated/invalid input
      for (uint32_t b = at; b < i && b < wordLen; ++b) {
        charIndex[b] = static_cast<uint8_t>(charCount);
      }
      ++charCount;
    }
  }
  marked[wordLen + 1] = '.';
  const uint32_t markedLen = wordLen + 2;
  if (charCount < kLeftMin + kRightMin) return 0;

  uint8_t values[kMaxWord + 3];
  memset(values, 0, sizeof(values));

  // For every start position, narrow a sorted range one character at a time;
  // an exact-length match applies its inter-character values (max-merge).
  for (uint32_t start = 0; start < markedLen; ++start) {
    uint32_t lo = 0;
    uint32_t hi = patternCount_;
    const uint32_t maxLen =
        markedLen - start < maxPatLen_ ? markedLen - start : maxPatLen_;
    for (uint32_t depth = 0; depth < maxLen && lo < hi; ++depth) {
      const uint8_t c = marked[start + depth];
      // Narrow [lo, hi) to keys with key[depth] == c (all share the first
      // `depth` characters already).
      uint32_t a = lo;
      uint32_t b = hi;
      while (a < b) {  // first key with key[depth] >= c (or shorter keys first)
        const uint32_t mid = (a + b) / 2;
        uint8_t klen;
        const uint8_t* key = keyAt(mid, &klen);
        if (klen <= depth || key[depth] < c) {
          a = mid + 1;
        } else {
          b = mid;
        }
      }
      lo = a;
      b = hi;
      while (a < b) {  // first key with key[depth] > c
        const uint32_t mid = (a + b) / 2;
        uint8_t klen;
        const uint8_t* key = keyAt(mid, &klen);
        if (klen <= depth || key[depth] <= c) {
          a = mid + 1;
        } else {
          b = mid;
        }
      }
      hi = a;
      if (lo >= hi) break;

      // Exact match of length depth+1 sits at the range start (sorted order
      // puts shorter keys before their extensions).
      uint8_t klen;
      const uint8_t* key = keyAt(lo, &klen);
      if (klen == depth + 1) {
        const uint8_t* vals = key + klen;  // klen+1 values follow the key
        for (uint32_t v = 0; v <= klen; ++v) {
          if (vals[v] > values[start + v]) values[start + v] = vals[v];
        }
      }
    }
  }

  // values index v corresponds to a break between marked[v-1] and marked[v];
  // map back to word byte offsets and apply the typesetting minimums in
  // CHARACTERS. Breaks land only on character starts (multi-byte patterns
  // carry zeros at continuation-byte positions by construction).
  uint8_t found = 0;
  for (uint32_t v = 1; v < markedLen - 1 && found < outCap; ++v) {
    const uint32_t wordPos = v - 1;  // break BEFORE word[wordPos]
    if ((values[v] & 1) == 0) continue;
    if ((static_cast<uint8_t>(word[wordPos]) & 0xC0) == 0x80) continue;  // mid-codepoint
    const uint32_t leftChars = charIndex[wordPos];
    if (leftChars < kLeftMin || charCount - leftChars < kRightMin) continue;
    out[found++] = static_cast<uint8_t>(wordPos);
  }
  return found;
}

}  // namespace book
}  // namespace freeink
