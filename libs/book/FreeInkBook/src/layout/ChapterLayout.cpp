// FreeInkBook — streaming SAX → block flow → measured lines → placed pages.
//
// Phase 4 shape: paragraphs are laid out in two phases. MEASURE breaks the
// paragraph into line records (libunibreak opportunities, kerning-aware
// widths, hyphenation at overflow); PLACE walks those records onto pages
// with widow/orphan control and resolves alignment (including justification
// as word-level runs with distributed space). Styles come from a per-element
// stack fed by element defaults, the book stylesheet, and inline style
// attributes. Memory remains O(paragraph + page).

#include "layout/ChapterLayout.h"

#include <new>

#include "BookProfile.h"

#include <stdio.h>
#include <string.h>

#include <linebreak.h>

#include "epub/ImageProbe.h"
#include "epub/PackageParsers.h"
#include "epub/XmlSax.h"
#include "text/arab_shaping.h"

namespace freeink {
namespace book {

namespace {

// Fixed working-set sizes. These bound layout memory regardless of chapter
// size; a paragraph longer than the buffer is flushed in segments (a layout
// artifact for pathological paragraphs, never a failure).
//
// The build profile (BookProfile.h) sizes the tiers: SMALL for PSRAM-less
// MCUs (fixed layout buffers ~30 KB), STANDARD for PSRAM parts (~62 KB),
// LARGE for generous-RAM targets (headroom for pathological books; pages
// virtually never split early on capacity).
#if FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_SMALL
// The small tier is sized against its real hosts: a deflated chapter's
// working set (these buffers + a 47 KB inflate stream + the cache writer's
// index) must fit a single allocation on ESP32-C3-class heaps where ~110 KB
// contiguous is the practical ceiling. Every KB here is a KB of books that
// do or don't open.
constexpr uint32_t kParTextCap = 3072;  // x3 buffers (text/breaks/levels); long paragraphs segment-flush
constexpr uint16_t kMaxSpans = 96;
constexpr uint16_t kMaxRunsPerPage = 304;
constexpr uint16_t kMaxLinesPerPar = 176;
constexpr uint32_t kPageArenaCap = 7 * 1024;
constexpr uint8_t kMaxElemDepth = 16;
constexpr uint16_t kMaxImagesPerPage = 8;
constexpr uint16_t kMaxLinksPerPage = 16;
constexpr uint8_t kMaxLinksPerPar = 6;
constexpr uint32_t kStyleTextCap = 3 * 1024;
constexpr uint16_t kMaxProbedImages = 96;
#elif FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_LARGE
constexpr uint32_t kParTextCap = 16384;
constexpr uint16_t kMaxSpans = 192;
constexpr uint16_t kMaxRunsPerPage = 1024;
constexpr uint16_t kMaxLinesPerPar = 1024;
constexpr uint32_t kPageArenaCap = 48 * 1024;
constexpr uint8_t kMaxElemDepth = 32;
constexpr uint16_t kMaxImagesPerPage = 24;
constexpr uint16_t kMaxLinksPerPage = 48;
constexpr uint8_t kMaxLinksPerPar = 12;
constexpr uint32_t kStyleTextCap = 24 * 1024;
constexpr uint16_t kMaxProbedImages = 512;
#else
constexpr uint32_t kParTextCap = 8192;
constexpr uint16_t kMaxSpans = 128;
constexpr uint16_t kMaxRunsPerPage = 768;  // word-level runs when justified
constexpr uint16_t kMaxLinesPerPar = 512;
constexpr uint32_t kPageArenaCap = 24 * 1024;
constexpr uint8_t kMaxElemDepth = 24;
constexpr uint16_t kMaxImagesPerPage = 16;
constexpr uint16_t kMaxLinksPerPage = 24;
constexpr uint8_t kMaxLinksPerPar = 8;
constexpr uint32_t kStyleTextCap = 12 * 1024;  // chapter-embedded <style> blocks
constexpr uint16_t kMaxProbedImages = 256;
#endif

// --- image dimension pre-scan ---------------------------------------------------
//
// Layout needs each <img>'s intrinsic dimensions to reserve space, but probing
// mid-parse opens a second inflate stream (~47 KB) while the chapter's own
// parse stream is live — the image-chapter memory peak. Instead the chapter is
// pre-scanned once (a collector parse gathering resolved image hrefs), the
// probes run sequentially with only one stream alive at a time, and the main
// layout parse reads dimensions from this table. Entries carry the FNV-1a
// hash of the resolved container path (the same hash the ZIP catalog keys
// on), not the path itself, so image-dense chapters fit in a few KB. The
// inline probe survives only as a fallback for table overflow.
struct ProbedImage {
  uint32_t hrefHash;  // ZipCatalog::hashPath of the resolved container path
  ImageInfo info;
};

// LineRec flags.
constexpr uint8_t kLineLast = 1u << 0;    // paragraph-final or hard break — never justify
constexpr uint8_t kLineHyphen = 1u << 1;  // render a hyphen after the last run

const char* localName(const char* qname) {
  const char* colon = strrchr(qname, ':');
  return colon != nullptr ? colon + 1 : qname;
}

const char* attrLocal(const char** atts, const char* local) {
  for (int i = 0; atts != nullptr && atts[i] != nullptr; i += 2) {
    const char* colon = strrchr(atts[i], ':');
    const char* name = colon != nullptr ? colon + 1 : atts[i];
    if (strcmp(name, local) == 0) return atts[i + 1];
  }
  return nullptr;
}

// Counts UTF-8 codepoints in text[0..len).
uint32_t countChars(const char* text, uint32_t len) {
  uint32_t chars = 0;
  for (uint32_t i = 0; i < len; ++i) {
    if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) ++chars;
  }
  return chars;
}

// Decodes the UTF-8 codepoint starting at text[i]; advances i past it.
uint32_t decodeUtf8(const char* text, uint32_t len, uint32_t& i) {
  const uint8_t b0 = static_cast<uint8_t>(text[i]);
  uint32_t cp = b0;
  uint32_t extra = 0;
  if (b0 >= 0xF0) {
    cp = b0 & 0x07;
    extra = 3;
  } else if (b0 >= 0xE0) {
    cp = b0 & 0x0F;
    extra = 2;
  } else if (b0 >= 0xC0) {
    cp = b0 & 0x1F;
    extra = 1;
  }
  ++i;
  while (extra > 0 && i < len && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) {
    cp = (cp << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
    ++i;
    --extra;
  }
  return cp;
}

// Encodes cp as UTF-8; returns bytes written (1-4).
uint32_t encodeUtf8(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// Letters that hyphenation patterns cover: Latin (with accents and the
// extended blocks), Greek, Cyrillic. Multiplication/division signs sit in
// the middle of Latin-1 and are excluded.
bool isHyphLetter(uint32_t cp) {
  if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return true;
  if (cp == 0x00D7 || cp == 0x00F7) return false;
  return (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x0370 && cp <= 0x03FF) ||
         (cp >= 0x0400 && cp <= 0x04FF);
}

// CJK ideographs, kana, Hangul, fullwidth forms, and the supplementary
// ideographic planes — the scripts that justify by inter-character expansion
// and take a quarter-em gap against Latin runs.
bool isCjk(uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0x20000 && cp <= 0x3FFFF);
}

bool isLatinWordChar(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

// Hangul: syllables plus jamo. Korean is the spaced CJK script — it breaks
// between syllables but justifies at word spaces, and attaches particles
// directly to Latin words ("TV를"), so it opts out of both the quarter-em
// rule and (when the line has spaces) inter-character justification.
bool isHangul(uint32_t cp) {
  return (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) ||
         (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0xD7B0 && cp <= 0xD7FF);
}

// A script boundary that conventionally gets a quarter-em of air (Japanese
// typesetting practice for Latin words embedded in CJK text). Korean does
// not follow it.
bool crossesScripts(uint32_t a, uint32_t b) {
  return (isCjk(a) && !isHangul(a) && isLatinWordChar(b)) ||
         (isLatinWordChar(a) && isCjk(b) && !isHangul(b));
}

// Full-width punctuation carries a built-in half-em of space — trailing for
// closers/stops, leading for openers. When two land adjacent (。」 or 」（)
// JLREQ compresses the pair by half an em; fonts render each glyph in a
// full em, so layout removes the overlap between them.
bool cjTrailingHalf(uint32_t cp) {
  switch (cp) {
    case 0x3001: case 0x3002:                                // 、。
    case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B:      // ，．：；
    case 0x3009: case 0x300B: case 0x300D: case 0x300F:      // 〉》」』
    case 0x3011: case 0x3015: case 0x3017: case 0x3019: case 0x301B:
    case 0xFF09: case 0xFF3D: case 0xFF5D:                   // ）］｝
      return true;
    default:
      return false;
  }
}

bool cjLeadingHalf(uint32_t cp) {
  switch (cp) {
    case 0x3008: case 0x300A: case 0x300C: case 0x300E:      // 〈《「『
    case 0x3010: case 0x3014: case 0x3016: case 0x3018: case 0x301A:
    case 0xFF08: case 0xFF3B: case 0xFF5B:                   // （［｛
      return true;
    default:
      return false;
  }
}

// Pixels removed between an adjacent full-width punctuation pair (0 when
// the pair does not compress). Exactly one half-em comes out: the closer's
// trailing space, or failing that the opener's leading space.
int32_t cjkCompression(uint32_t prev, uint32_t cp, uint16_t sizePx) {
  const bool prevPunct = cjTrailingHalf(prev) || cjLeadingHalf(prev);
  const bool curPunct = cjTrailingHalf(cp) || cjLeadingHalf(cp);
  if (!prevPunct || !curPunct) return 0;
  if (cjTrailingHalf(prev) || cjLeadingHalf(cp)) return -(static_cast<int32_t>(sizePx) / 2);
  return 0;
}

// --- Bidi (UAX #9 subset for book text) ---------------------------------------
//
// Hebrew-class RTL: strong-R runs, embedded LTR words/numbers, mirrored
// brackets. Levels 0-2 cover real books (base direction + one embedding).
// Arabic additionally gets contextual joining forms (see shapeArabic below).

enum : uint8_t { kBidiL = 0, kBidiR = 1, kBidiEN = 2, kBidiNeutral = 3 };

uint8_t bidiClass(uint32_t cp) {
  if ((cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0xFB1D && cp <= 0xFB4F)) return kBidiR;  // Hebrew
  if ((cp >= 0x0600 && cp <= 0x07BF) || (cp >= 0x08A0 && cp <= 0x08FF) ||
      (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) {
    return kBidiR;  // Arabic block (reorder-only; no shaping)
  }
  if (cp >= '0' && cp <= '9') return kBidiEN;
  if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F) ||
      (cp >= 0x0370 && cp <= 0x058F) || cp >= 0x2E80) {
    return kBidiL;  // Latin/Greek/Cyrillic/CJK — strong L for our purposes
  }
  return kBidiNeutral;
}

// Fills levels[byte] for every byte of text (continuation bytes copy their
// lead). Returns true when the paragraph base direction is RTL (first strong
// character is R). Levels: base 0/1; opposite-direction strong runs and
// numbers inside RTL get base+1 (numbers in RTL → 2). Sets *hasArabic when
// any codepoint needs joining analysis (shapeArabic).
bool computeBidiLevels(const char* text, uint32_t len, uint8_t* levels, bool* hasArabic) {
  // P2/P3: base = first strong.
  bool baseRtl = false;
  {
    uint32_t i = 0;
    while (i < len) {
      uint32_t j = i;
      const uint32_t cp = decodeUtf8(text, len, j);
      const uint8_t cls = bidiClass(cp);
      if (cls == kBidiR) { baseRtl = true; break; }
      if (cls == kBidiL) break;
      i = j;
    }
  }
  const uint8_t base = baseRtl ? 1 : 0;
  // First pass: strong/EN levels, neutrals marked 0xFF.
  uint32_t i = 0;
  while (i < len) {
    const uint32_t start = i;
    const uint32_t cp = decodeUtf8(text, len, i);
    if (cp >= kArabJoinLo && cp <= kArabJoinHi) *hasArabic = true;
    const uint8_t cls = bidiClass(cp);
    uint8_t level;
    if (cls == kBidiR) level = 1;                       // R is always odd
    else if (cls == kBidiL) level = base == 0 ? 0 : 2;  // L inside RTL embeds
    else if (cls == kBidiEN) level = base == 0 ? 0 : 2; // numbers read LTR
    else level = 0xFF;
    for (uint32_t b = start; b < i; ++b) levels[b] = level;
  }
  // Second pass: neutrals take the surrounding level when both sides agree,
  // else the base level (N1/N2 simplified).
  uint32_t pos = 0;
  uint8_t prev = base;
  while (pos < len) {
    if (levels[pos] != 0xFF) { prev = levels[pos]; ++pos; continue; }
    uint32_t end = pos;
    while (end < len && levels[end] == 0xFF) ++end;
    uint8_t next = base;
    if (end < len) next = levels[end];
    const uint8_t fill = prev == next ? prev : base;
    for (uint32_t b = pos; b < end; ++b) levels[b] = fill;
    pos = end;
  }
  return baseRtl;
}

// UAX #9 L4: mirrored characters in RTL runs.
uint32_t bidiMirror(uint32_t cp) {
  switch (cp) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0x00AB: return 0x00BB;  // « »
    case 0x00BB: return 0x00AB;
    default: return cp;
  }
}

// UAX #9 L2 at character granularity: an odd-level segment's codepoints are
// stored in visual order so the renderer's left-to-right blit is correct.
// Classic in-place trick: reverse all bytes, then re-reverse each multibyte
// sequence (a continuation byte now precedes its lead byte).
void reverseUtf8(char* s, uint32_t len) {
  for (uint32_t a = 0, b = len - 1; a < b; ++a, --b) {
    const char t = s[a];
    s[a] = s[b];
    s[b] = t;
  }
  uint32_t i = 0;
  while (i < len) {
    if ((static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) {
      uint32_t j = i;
      while (j + 1 < len && (static_cast<uint8_t>(s[j + 1]) & 0xC0) == 0x80) ++j;
      if (j + 1 < len) ++j;  // include the lead byte
      for (uint32_t a = i, b = j; a < b; ++a, --b) {
        const char t = s[a];
        s[a] = s[b];
        s[b] = t;
      }
      i = j + 1;
    } else {
      ++i;
    }
  }
}

// --- Arabic joining (contextual forms) -----------------------------------------
//
// Arabic letters connect: each takes an isolated/initial/medial/final glyph
// depending on whether it actually joins its neighbors (Unicode ch. 9).
// Every contextual glyph exists as a codepoint in the Presentation Forms
// blocks, so shaping is a substitution — resolved here once per paragraph,
// applied in decodeShaped() during measurement, and baked into page-run text
// exactly like ligatures. The renderer needs no shaping logic; it does need
// a font with Presentation Forms coverage (BookFont::covers guards each
// substitution, so fonts without them degrade to today's isolated letters).
//
// The resolved form is packed into the levels array above the bidi level:
// bits 0-2 bidi level, bits 3-5 form.
enum : uint8_t {
  kBidiLevelMask = 0x07,
  kFocusBold = 0x40,  // focus reading: byte belongs to a word's bold prefix
  kFormShift = 3,
  kFormNone = 0,
  kFormIso = 1,
  kFormFin = 2,
  kFormIni = 3,
  kFormMed = 4,
  kFormLamAlefIso = 5,  // on the lam; the following alef is kFormSkip
  kFormLamAlefFin = 6,
  kFormSkip = 7,        // alef consumed by a lam-alef ligature
};

uint8_t arabJoin(uint32_t cp) {
  if (cp >= kArabJoinLo && cp <= kArabJoinHi) return kArabJoinType[cp - kArabJoinLo];
  return kJoinU;
}

const ArabForms* arabFormsFor(uint32_t cp) {
  uint32_t lo = 0, hi = kArabFormsCount;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (kArabForms[mid].base < cp) lo = mid + 1;
    else hi = mid;
  }
  return lo < kArabFormsCount && kArabForms[lo].base == cp ? &kArabForms[lo] : nullptr;
}

const ArabLamAlef* lamAlefFor(uint32_t alefCp) {
  for (uint32_t i = 0; i < kArabLamAlefCount; ++i) {
    if (kArabLamAlef[i].alef == alefCp) return &kArabLamAlef[i];
  }
  return nullptr;
}

// The presentation-form codepoint for (base letter, resolved form), or 0.
uint32_t arabPresentation(uint32_t cp, uint8_t form) {
  const ArabForms* f = arabFormsFor(cp);
  if (f == nullptr || form < kFormIso || form > kFormMed) return 0;
  return f->forms[form - kFormIso];
}

// One logical pass over the paragraph: resolve each Arabic letter's form
// from whether it connects to its neighbors (transparent marks are skipped
// per the joining rules), and fuse lam + immediately-following alef into the
// mandatory ligature — but only when `font` covers the ligature glyph, since
// the consumed alef cannot be resurrected at render time. Vowel marks
// between lam and alef defeat the ligature (v1: adjacency required).
void shapeArabic(const char* text, uint32_t len, uint8_t* levels, BookFont* font) {
  bool prevJoins = false;  // does the previous letter connect forward?
  uint32_t i = 0;
  while (i < len) {
    const uint32_t start = i;
    const uint32_t cp = decodeUtf8(text, len, i);
    const uint8_t jt = arabJoin(cp);
    if (jt == kJoinT) continue;  // marks render as-is and keep the join chain
    const ArabForms* forms = arabFormsFor(cp);
    if (forms == nullptr) {
      // No contextual glyphs (tatweel, punctuation, non-Arabic): only the
      // joining chain matters.
      prevJoins = jt == kJoinC || jt == kJoinD || jt == kJoinL;
      continue;
    }
    // Lam + alef fuses when the font can draw the ligature.
    if (cp == 0x0644 && i < len) {
      uint32_t j = i;
      const uint32_t alef = decodeUtf8(text, len, j);
      const ArabLamAlef* la = lamAlefFor(alef);
      if (la != nullptr) {
        const uint8_t form = prevJoins ? kFormLamAlefFin : kFormLamAlefIso;
        const uint32_t lig = prevJoins ? la->final : la->isolated;
        if (font->covers(lig)) {
          for (uint32_t b = start; b < i; ++b)
            levels[b] = static_cast<uint8_t>(levels[b] | (form << kFormShift));
          for (uint32_t b = i; b < j; ++b)
            levels[b] = static_cast<uint8_t>(levels[b] | (kFormSkip << kFormShift));
          i = j;
          prevJoins = false;  // the ligature does not join forward (alef is R)
          continue;
        }
      }
    }
    // Forward context: the next non-transparent character must accept a
    // join on its trailing side (D/R/C).
    bool nextAccepts = false;
    for (uint32_t j = i; j < len;) {
      const uint32_t ncp = decodeUtf8(text, len, j);
      const uint8_t njt = arabJoin(ncp);
      if (njt == kJoinT) continue;
      nextAccepts = njt == kJoinD || njt == kJoinR || njt == kJoinC;
      break;
    }
    const bool joinsPrev = prevJoins && (jt == kJoinD || jt == kJoinR);
    const bool joinsNext = jt == kJoinD && nextAccepts;
    uint8_t form;
    if (joinsPrev && joinsNext) form = kFormMed;
    else if (joinsPrev) form = kFormFin;
    else if (joinsNext) form = kFormIni;
    else form = kFormIso;
    for (uint32_t b = start; b < i; ++b)
      levels[b] = static_cast<uint8_t>(levels[b] | (form << kFormShift));
    prevJoins = jt == kJoinD;
  }
}

// Focus reading (CrossPoint parity): bold the first ~45% of each word's
// characters (clamped to 1..9) as a fixation anchor. Letters and apostrophes
// form words; digits, punctuation, and CJK stay regular. Marked as a bit in
// the levels array; measurement and emission add StyleBold for marked bytes.
bool isFocusWordChar(uint32_t cp) {
  return isHyphLetter(cp) || cp == '\'' || cp == 0x2018 || cp == 0x2019;
}

void markFocusWords(const char* text, uint32_t len, uint8_t* levels) {
  uint32_t i = 0;
  while (i < len) {
    const uint32_t wordStart = i;
    uint32_t chars = 0;
    uint32_t probe = i;
    while (probe < len) {
      uint32_t next = probe;
      const uint32_t cp = decodeUtf8(text, len, next);
      if (!isFocusWordChar(cp)) break;
      probe = next;
      ++chars;
    }
    if (chars == 0) {
      decodeUtf8(text, len, i);
      continue;
    }
    uint32_t bold = chars * 45 / 100;
    if (bold < 1) bold = 1;
    if (bold > 9) bold = 9;
    uint32_t j = wordStart;
    for (uint32_t c = 0; c < bold; ++c) {
      const uint32_t at = j;
      decodeUtf8(text, len, j);
      for (uint32_t b = at; b < j; ++b) {
        levels[b] = static_cast<uint8_t>(levels[b] | kFocusBold);
      }
    }
    i = probe;
  }
}

// User-agent defaults per element, expressed as CSS so the book's own CSS
// cascades over them naturally. Margins are % of em.
CssDecl elementDefaults(const char* local) {
  CssDecl d;
  if (strcmp(local, "h1") == 0) {
    d.sizePct = 200;
    d.weightBold = 1;
    d.marginTopPct = 100;
    d.marginBottomPct = 50;
  } else if (strcmp(local, "h2") == 0) {
    d.sizePct = 160;
    d.weightBold = 1;
    d.marginTopPct = 90;
    d.marginBottomPct = 45;
  } else if (strcmp(local, "h3") == 0) {
    d.sizePct = 130;
    d.weightBold = 1;
    d.marginTopPct = 80;
    d.marginBottomPct = 40;
  } else if (strcmp(local, "h4") == 0 || strcmp(local, "h5") == 0 || strcmp(local, "h6") == 0) {
    d.sizePct = 115;
    d.weightBold = 1;
    d.marginTopPct = 70;
    d.marginBottomPct = 35;
  } else if (strcmp(local, "blockquote") == 0) {
    d.styleItalic = 1;
    d.marginTopPct = 40;
    d.marginBottomPct = 40;
  } else if (strcmp(local, "li") == 0) {
    d.marginTopPct = 10;
    d.marginBottomPct = 10;
  } else if (strcmp(local, "p") == 0 || strcmp(local, "div") == 0) {
    d.marginBottomPct = 40;
  } else if (strcmp(local, "b") == 0 || strcmp(local, "strong") == 0) {
    d.weightBold = 1;
  } else if (strcmp(local, "i") == 0 || strcmp(local, "em") == 0 || strcmp(local, "cite") == 0) {
    d.styleItalic = 1;
  } else if (strcmp(local, "small") == 0) {
    d.sizePct = 80;
  } else if (strcmp(local, "big") == 0) {
    d.sizePct = 120;
  } else if (strcmp(local, "sub") == 0) {
    d.sizePct = 50;
    d.vertAlign = 2;
  } else if (strcmp(local, "sup") == 0) {
    d.sizePct = 50;
    d.vertAlign = 1;
  } else if (strcmp(local, "u") == 0 || strcmp(local, "ins") == 0) {
    d.underline = 1;
  }
  return d;
}

int16_t blockIndentFor(const char* local, uint16_t baseSizePx) {
  if (strcmp(local, "blockquote") == 0) return static_cast<int16_t>(baseSizePx * 2);
  if (strcmp(local, "li") == 0) return static_cast<int16_t>(baseSizePx * 3 / 2);
  return 0;
}

bool isBlockElement(const char* local) {
  static const char* kBlocks[] = {"p",       "h1",      "h2",         "h3",    "h4",
                                  "h5",      "h6",      "blockquote", "li",    "div",
                                  "section", "article", "figure",     "aside", "figcaption",
                                  "ul",      "ol",      "table",      "tr",    "td",
                                  "th",      "dt",      "dd"};
  for (const char* b : kBlocks) {
    if (strcmp(local, b) == 0) return true;
  }
  return false;
}

bool isSuppressedElement(const char* local) {
  return strcmp(local, "head") == 0 || strcmp(local, "style") == 0 ||
         strcmp(local, "script") == 0 || strcmp(local, "title") == 0;
}

// Cheap detector: does the raw chapter markup mention an image element at
// all? Streams the entry once (inflate only, no XML parse) looking for "<im"
// (matches <img, <image) or ":im" (prefixed forms like <svg:image). False
// positives just cost one collector parse; false negatives cannot occur for
// elements the layout engine would match (XML names are case-sensitive and
// the engine matches lowercase img/image only).
bool entryMentionsImages(BookSource& source, const ZipEntry& entry, Arena& scratch) {
  const size_t marked = scratch.mark();
  ZipEntryReader reader;
  if (reader.open(source, entry, scratch) != BookStatus::Ok) {
    scratch.release(marked);
    return true;  // let the collector parse (and then layout) surface errors
  }
  char* buf = static_cast<char*>(scratch.alloc(2048, 1));
  if (buf == nullptr) {
    scratch.release(marked);
    return true;
  }
  bool found = false;
  char carry[2] = {0, 0};
  while (!found) {
    const int32_t n = reader.read(buf, 2048);
    if (n <= 0) break;
    for (int32_t i = 0; i < n && !found; ++i) {
      const char c2 = i >= 2 ? buf[i - 2] : carry[i];
      const char c1 = i >= 1 ? buf[i - 1] : carry[i + 1];
      if (c1 == 'i' && buf[i] == 'm' && (c2 == '<' || c2 == ':')) found = true;
    }
    if (n >= 2) {
      carry[0] = buf[n - 2];
      carry[1] = buf[n - 1];
    } else {
      carry[0] = carry[1];
      carry[1] = buf[0];
    }
  }
  scratch.release(marked);
  return found;
}

// First pass over an image-bearing chapter: collect each unique <img>/<image>
// target (resolved to its container path) into the caller's table so the
// dimension probes can run after this parse stream closes.
class ImageCollector : public XmlHandler {
 public:
  ImageCollector(const char* chapterHref, Arena& scratch, ProbedImage* table, uint16_t cap)
      : scratch_(scratch), table_(table), cap_(cap) {
    if (!dirName(chapterHref != nullptr ? chapterHref : "", chapterDir_, sizeof(chapterDir_))) {
      chapterDir_[0] = '\0';
    }
  }

  void onStartElement(const char* name, const char** atts) override {
    const char* local = localName(name);
    if (strcmp(local, "img") != 0 && strcmp(local, "image") != 0) return;
    const char* src = attrLocal(atts, "src");
    if (src == nullptr) src = attrLocal(atts, "href");  // SVG xlink:href
    if (src == nullptr) return;
    const size_t marked = scratch_.mark();
    const char* resolved = resolveHref(scratch_, chapterDir_, src, nullptr);
    if (resolved != nullptr) {
      const uint32_t hash = ZipCatalog::hashPath(resolved);
      bool known = false;
      for (uint16_t i = 0; i < count_ && !known; ++i) {
        known = table_[i].hrefHash == hash;
      }
      if (!known) {
        table_[count_].hrefHash = hash;
        table_[count_].info = ImageInfo{};
        if (++count_ >= cap_) stopParse = true;  // rest fall back to inline probes
      }
    }
    scratch_.release(marked);
  }

  uint16_t count() const { return count_; }

 private:
  Arena& scratch_;
  ProbedImage* table_;
  uint16_t cap_;
  uint16_t count_ = 0;
  char chapterDir_[512];
};

class LayoutEngine : public XmlHandler {
 public:
  LayoutEngine(BookSource& source, const ZipCatalog* zip, const char* chapterHref,
               const LayoutParams& params, Arena& scratch, PageSink& sink,
               const ProbedImage* probed = nullptr, uint16_t probedCount = 0,
               Arena* parseArena = nullptr)
      : source_(source), zip_(zip), params_(params), scratch_(scratch),
        parseArena_(parseArena != nullptr ? *parseArena : scratch), sink_(sink),
        probed_(probed), probedCount_(probedCount) {
    if (!dirName(chapterHref != nullptr ? chapterHref : "", chapterDir_, sizeof(chapterDir_))) {
      chapterDir_[0] = '\0';
    }
  }

  bool init() {
    parText_ = static_cast<char*>(scratch_.alloc(kParTextCap, 1));
    breaks_ = static_cast<char*>(scratch_.alloc(kParTextCap, 1));
    levels_ = static_cast<uint8_t*>(scratch_.alloc(kParTextCap, 1));
    spans_ = scratch_.allocArray<Span>(kMaxSpans);
    runs_ = scratch_.allocArray<PageTextRun>(kMaxRunsPerPage);
    images_ = scratch_.allocArray<PageImage>(kMaxImagesPerPage);
    links_ = scratch_.allocArray<PageLink>(kMaxLinksPerPage);
    lines_ = scratch_.allocArray<LineRec>(kMaxLinesPerPar);
    // Page-run text lives in its own sub-arena, NOT in `scratch_`: the XML
    // parse that drives this engine keeps its inflate window and parser
    // buffers live in `scratch_` for the whole chapter, and those are
    // allocated *after* init() runs. Releasing per-page copies from the
    // shared arena would free the decompressor out from under the reader.
    styleText_ = static_cast<char*>(scratch_.alloc(kStyleTextCap, 1));
    chapterCssOk_ = styleText_ != nullptr && chapterBuilder_.begin(scratch_);
    void* pageBlock = scratch_.alloc(kPageArenaCap, alignof(max_align_t));
    if (parText_ == nullptr || breaks_ == nullptr || levels_ == nullptr || spans_ == nullptr || runs_ == nullptr ||
        images_ == nullptr || links_ == nullptr || lines_ == nullptr || pageBlock == nullptr) {
      return false;
    }
    pageArena_.init(pageBlock, kPageArenaCap);
    pageY_ = params_.marginTop;

    ElemState root;
    root.sizePct = 100;
    root.flags = StyleNone;
    root.align = params_.defaultAlign;
    root.textIndentPct = 0;
    root.displayNone = false;
    stack_[0] = root;
    stackTop_ = 0;

    latchParagraph("p", CssDecl{});
    return true;
  }

  // --- XmlHandler ----------------------------------------------------------

  void onStartElement(const char* name, const char** atts) override {
    if (failed_ || stopParse) return;
    const char* local = localName(name);
    if (isSuppressedElement(local)) {
      ++suppress_;
      if (strcmp(local, "style") == 0) ++inStyle_;  // captured, not flowed
      return;
    }
    if (strcmp(local, "body") == 0) {
      inBody_ = true;
      // Books style body{} heavily (font-size, text-align); apply it as the
      // root of the element stack.
      if (params_.stylesheet != nullptr) {
        CssDecl bodyDecl = elementDefaults("body");
        bodyDecl.applyOver(cascadeFor(*params_.stylesheet, "body", attrLocal(atts, "class"),
                                      nullptr));
        pushState(bodyDecl);
        latchParagraphFromStack();
      }
      return;
    }
    ++depth_;
    if (!inBody_ || suppress_ > 0) return;

    // Resolve this element's style: defaults ← book CSS ← inline style.
    CssDecl decl = elementDefaults(local);
    const char* styleAttr = attrLocal(atts, "style");
    CssDecl inlineDecl;
    bool hasInline = false;
    if (styleAttr != nullptr) {
      inlineDecl = parseInlineStyle(styleAttr);
      hasInline = true;
    }
    const char* cls = attrLocal(atts, "class");
    if (params_.stylesheet != nullptr) {
      decl.applyOver(cascadeFor(*params_.stylesheet, local, cls, nullptr));
    }
    if (chapterSheet_.ruleCount > 0) {
      decl.applyOver(cascadeFor(chapterSheet_, local, cls, nullptr));
    }
    if (hasInline) decl.applyOver(inlineDecl);
    pushState(decl);
    if (decl.marginLeftPct >= 0) {
      pendingMarginRootPct_ += static_cast<uint32_t>(currentSizePct()) *
                               static_cast<uint16_t>(decl.marginLeftPct) / 100;
      noteStyleChange();
    }

    // id="" anchors: record the chapter char offset for link/footnote jumps.
    if (const char* id = attrLocal(atts, "id")) {
      sink_.onAnchor(ZipCatalog::hashPath(id), parCharBase_ + countChars(parText_, parLen_));
    }
    if (strcmp(local, "a") == 0) {
      const char* href = attrLocal(atts, "href");
      if (href != nullptr && parLinkCount_ < kMaxLinksPerPar) {
        const size_t m = scratch_.mark();
        const char* frag = nullptr;
        const char* path = resolveHref(scratch_, chapterDir_, href, &frag);
        if (path != nullptr) {
          snprintf(parLinkPath_[parLinkCount_], sizeof(parLinkPath_[0]), "%s", path);
          snprintf(parLinkFrag_[parLinkCount_], sizeof(parLinkFrag_[0]), "%s",
                   frag != nullptr ? frag : "");
          currentLink_ = static_cast<uint8_t>(++parLinkCount_);
          noteStyleChange();
        }
        scratch_.release(m);
      }
    }

    if (isBlockElement(local)) {
      flushParagraph();
      latchParagraph(local, decl);
    } else if (strcmp(local, "br") == 0) {
      appendRaw('\n');
    } else if (strcmp(local, "hr") == 0) {
      flushParagraph();
      advanceY(lineHeightFor(params_.baseSizePx));
    } else if (strcmp(local, "img") == 0 || strcmp(local, "image") == 0) {
      const char* src = attrLocal(atts, "src");
      if (src == nullptr) src = attrLocal(atts, "href");  // SVG xlink:href
      if (src != nullptr && !stack_[stackTop_].displayNone) {
        flushParagraph();
        placeImage(src);
      }
    } else {
      noteStyleChange();  // inline element may have changed flags/size
    }
  }

  void onEndElement(const char* name) override {
    if (failed_ || stopParse) return;
    const char* local = localName(name);
    if (isSuppressedElement(local)) {
      if (suppress_ > 0) --suppress_;
      if (strcmp(local, "style") == 0 && inStyle_ > 0) {
        --inStyle_;
        if (chapterCssOk_ && styleLen_ > 0) {
          chapterBuilder_.addText(styleText_, styleLen_);
          chapterSheet_ = chapterBuilder_.finish();
          styleLen_ = 0;
        }
      }
      return;
    }
    if (strcmp(local, "body") == 0) {
      flushParagraph();
      inBody_ = false;
      return;
    }
    if (depth_ > 0) --depth_;
    if (!inBody_ || suppress_ > 0) return;
    popState();
    if (strcmp(local, "a") == 0 && currentLink_ != 0) {
      currentLink_ = 0;
      noteStyleChange();
    }

    if (isBlockElement(local)) {
      flushParagraph();
      // Text following the closed block flows in the parent's style with no
      // extra spacing.
      latchParagraphFromStack();
    } else {
      noteStyleChange();
    }
  }

  void onText(const char* text, int len) override {
    if (inStyle_ > 0 && chapterCssOk_ && params_.embeddedStyles) {
      for (int i = 0; i < len && styleLen_ + 1 < kStyleTextCap; ++i) styleText_[styleLen_++] = text[i];
      return;
    }
    if (failed_ || stopParse || !inBody_ || suppress_ > 0) return;
    if (stack_[stackTop_].displayNone) return;
    for (int i = 0; i < len; ++i) {
      const char c = text[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        pendingSpace_ = parLen_ > 0;  // collapse runs; drop leading whitespace
      } else {
        if (pendingSpace_) {
          appendRaw(' ');
          pendingSpace_ = false;
        }
        appendRaw(c);
      }
    }
  }

  // --- Completion ----------------------------------------------------------

  bool finish() {
    if (failed_) return false;
    flushParagraph();
    if (runCount_ > 0 || imageCount_ > 0) emitPage();
    return !failed_;
  }

  uint32_t pageCount() const { return pageCount_; }
  uint32_t totalChars() const { return parCharBase_; }
  bool outOfMemory() const { return failed_; }

 private:
  struct Span {
    uint16_t start;
    uint8_t flags;
    uint16_t sizePct;
    uint16_t marginBeforeRootPct;
    uint8_t link;  // 0 = none, else 1-based index into parLinks_
  };

  struct LineRec {
    uint32_t start;
    uint32_t end;
    int32_t naturalWidth;  // includes the hyphen when kLineHyphen is set
    uint16_t spaceCount;   // adjustable spaces (Latin justification)
    uint16_t cjkGaps;      // CJ boundaries (inter-character justification)
    uint16_t hangulGaps;   // Hangul boundaries — stretch only on space-less lines
    uint8_t maxSizePct;
    uint8_t flags;
  };

  struct ElemState {
    uint16_t sizePct;
    uint8_t flags;
    TextAlign align;
    int16_t textIndentPct;
    bool displayNone;
  };

  struct ParaStyle {
    uint16_t sizePct;
    uint8_t baseFlags;
    TextAlign align;
    int16_t indentPx;
    int16_t textIndentPct;
    uint16_t spaceBeforePct;
    uint16_t spaceAfterPct;
  };

  // --- style stack ----------------------------------------------------------

  void pushState(const CssDecl& decl) {
    const ElemState& parent = stack_[stackTop_];
    ElemState next = parent;
    if (decl.sizePct != 0) {
      const uint32_t scaled = decl.sizeRootRelative
                                  ? decl.sizePct
                                  : static_cast<uint32_t>(parent.sizePct) * decl.sizePct / 100;
      next.sizePct = static_cast<uint16_t>(scaled < 30 ? 30 : (scaled > 250 ? 250 : scaled));
    }
    if (decl.weightBold == 1) next.flags |= StyleBold;
    if (decl.weightBold == 0) next.flags &= static_cast<uint8_t>(~StyleBold);
    if (decl.styleItalic == 1) next.flags |= StyleItalic;
    if (decl.styleItalic == 0) next.flags &= static_cast<uint8_t>(~StyleItalic);
    if (decl.underline == 1) next.flags |= StyleUnderline;
    if (decl.underline == 0) next.flags &= static_cast<uint8_t>(~StyleUnderline);
    if (decl.vertAlign >= 0) {
      next.flags &= static_cast<uint8_t>(~(StyleSuperscript | StyleSubscript));
      if (decl.vertAlign == 1) next.flags |= StyleSuperscript;
      if (decl.vertAlign == 2) next.flags |= StyleSubscript;
    }
    if (decl.align != TextAlign::Inherit) next.align = decl.align;
    if (decl.textIndentPct >= 0) next.textIndentPct = decl.textIndentPct;
    if (decl.displayNone == 1) next.displayNone = true;
    if (stackTop_ + 1 < kMaxElemDepth) {
      stack_[++stackTop_] = next;
    } else {
      ++stackOverflow_;  // deeper levels inherit the top unchanged
    }
  }

  void popState() {
    if (stackOverflow_ > 0) {
      --stackOverflow_;
      return;
    }
    if (stackTop_ > 0) --stackTop_;
  }

  uint8_t currentFlags() const { return stack_[stackTop_].flags; }
  uint16_t currentSizePct() const { return stack_[stackTop_].sizePct; }

  uint16_t currentPendingMarginRootPct() const {
    return pendingMarginRootPct_ > 65535u ? 65535u : static_cast<uint16_t>(pendingMarginRootPct_);
  }

  // --- paragraph accumulation ------------------------------------------------

  void noteStyleChange() {
    if (spanCount_ == 0) return;  // paragraph text not started yet
    const uint8_t flags = currentFlags();
    const uint16_t sizePct = currentSizePct();
    const uint16_t marginBeforeRootPct = currentPendingMarginRootPct();
    Span& last = spans_[spanCount_ - 1];
    if (last.flags == flags && last.sizePct == sizePct && last.link == currentLink_ &&
        marginBeforeRootPct == 0) {
      return;
    }
    if (last.start == parLen_) {
      last.flags = flags;
      last.sizePct = sizePct;
      last.link = currentLink_;
      last.marginBeforeRootPct = static_cast<uint16_t>(
          last.marginBeforeRootPct + marginBeforeRootPct > 65535u
              ? 65535u
              : last.marginBeforeRootPct + marginBeforeRootPct);
      pendingMarginRootPct_ = 0;
    } else if (spanCount_ < kMaxSpans) {
      spans_[spanCount_++] = {static_cast<uint16_t>(parLen_), flags, sizePct,
                              marginBeforeRootPct, currentLink_};
      pendingMarginRootPct_ = 0;
    }
  }

  void appendRaw(char c) {
    if (parLen_ + 1 >= kParTextCap) {
      // Pathological paragraph: flush what we have and continue seamlessly.
      const ParaStyle style = para_;
      flushParagraph();
      para_ = style;
      para_.spaceBeforePct = 0;
      continued_ = true;  // continuation lines get no first-line indent
    }
    if (spanCount_ == 0) {
      spans_[spanCount_++] = {0, currentFlags(), currentSizePct(),
                              currentPendingMarginRootPct(), currentLink_};
      pendingMarginRootPct_ = 0;
    }
    parText_[parLen_++] = c;
  }

  void resetParagraphLinks() {
    parLinkCount_ = 0;
    currentLink_ = 0;
  }

  void latchParagraph(const char* local, const CssDecl& decl) {
    const ElemState& state = stack_[stackTop_];
    para_.sizePct = state.sizePct;
    para_.baseFlags = state.flags;
    para_.align = state.align;
    para_.indentPx = blockIndentFor(local, params_.baseSizePx);
    para_.textIndentPct = state.textIndentPct;
    para_.spaceBeforePct = decl.marginTopPct >= 0 ? static_cast<uint16_t>(decl.marginTopPct) : 0;
    para_.spaceAfterPct =
        decl.marginBottomPct >= 0 ? static_cast<uint16_t>(decl.marginBottomPct) : 0;
    resetParagraphLinks();
    parLen_ = 0;
    spanCount_ = 0;
    pendingMarginRootPct_ = 0;
    pendingSpace_ = false;
  }

  void latchParagraphFromStack() {
    const ElemState& state = stack_[stackTop_];
    para_.sizePct = state.sizePct;
    para_.baseFlags = state.flags;
    para_.align = state.align;
    para_.indentPx = 0;
    para_.textIndentPct = 0;
    para_.spaceBeforePct = 0;
    para_.spaceAfterPct = 0;
    resetParagraphLinks();
    parLen_ = 0;
    spanCount_ = 0;
    pendingMarginRootPct_ = 0;
    pendingSpace_ = false;
  }

  int16_t lineHeightFor(uint16_t sizePx) const {
    return static_cast<int16_t>(static_cast<int32_t>(params_.font->lineHeight(sizePx)) *
                                params_.lineSpacingPct / 100);
  }

  uint16_t sizePxForPct(uint16_t sizePct) const {
    const uint32_t size = static_cast<uint32_t>(params_.baseSizePx) * sizePct / 100;
    return static_cast<uint16_t>(size < 1 ? 1 : size);
  }

  uint16_t paragraphSizePx() const {
    return sizePxForPct(para_.sizePct);
  }

  uint16_t spanSizePx(const Span& span) const {
    return sizePxForPct(span.sizePct);
  }

  int32_t marginBeforePx(const Span& span) const {
    return static_cast<int32_t>(params_.baseSizePx) * span.marginBeforeRootPct / 100;
  }

  const Span& spanAt(uint32_t byteOffset) const {
    uint16_t found = 0;
    for (uint16_t s = 0; s < spanCount_ && spans_[s].start <= byteOffset; ++s) found = s;
    return spans_[found];
  }

  void advanceY(int16_t dy) {
    if (pageY_ > params_.marginTop) pageY_ += dy;  // no leading gaps at page top
  }

  // --- images ----------------------------------------------------------------

  void placeImage(const char* src) {
    const size_t marked = scratch_.mark();
    const char* resolved =
        zip_ != nullptr ? resolveHref(scratch_, chapterDir_, src, nullptr) : nullptr;
    ImageInfo info;
    bool prescanned = false;
    const uint32_t hrefHash = resolved != nullptr ? ZipCatalog::hashPath(resolved) : 0;
    for (uint16_t p = 0; resolved != nullptr && p < probedCount_ && !prescanned; ++p) {
      if (probed_[p].hrefHash == hrefHash) {
        info = probed_[p].info;
        prescanned = true;
      }
    }
    if (!prescanned && resolved != nullptr) {
      // Fallback (table overflow or over-long href): probe inline. This is
      // the old two-concurrent-streams path — rare by construction.
      const ZipEntry* entry = zip_->find(resolved);
      if (entry != nullptr) probeImage(source_, *entry, parseArena_, &info);
    }
    if (resolved == nullptr || info.kind == ImageInfo::Kind::Unknown || info.width == 0 ||
        info.height == 0) {
      scratch_.release(marked);  // unknown format or missing target — skip
      return;
    }

    // Fit within the content box, preserving aspect, never upscaling.
    const int32_t contentW = params_.pageWidth - params_.marginLeft - params_.marginRight;
    const int32_t contentH = params_.pageHeight - params_.marginTop - params_.marginBottom;
    int32_t w = info.width;
    int32_t h = info.height;
    if (w > contentW) {
      h = h * contentW / w;
      w = contentW;
    }
    if (h > contentH) {
      w = w * contentH / h;
      h = contentH;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    const bool hasContent = runCount_ > 0 || imageCount_ > 0 || pageY_ > params_.marginTop;
    if ((pageY_ + h > params_.pageHeight - params_.marginBottom || imageCount_ >= kMaxImagesPerPage) &&
        hasContent) {
      emitPage();
      if (stopParse) {
        scratch_.release(marked);
        return;
      }
    }

    const char* hrefCopy = pageArena_.strdup(resolved);
    scratch_.release(marked);
    if (hrefCopy == nullptr || imageCount_ >= kMaxImagesPerPage) {
      failed_ = hrefCopy == nullptr;
      return;
    }
    PageImage& img = images_[imageCount_++];
    img.href = hrefCopy;
    img.width = static_cast<uint16_t>(w);
    img.height = static_cast<uint16_t>(h);
    img.x = static_cast<int16_t>(params_.marginLeft + (contentW - w) / 2);
    img.y = pageY_;
    if (runCount_ == 0 && imageCount_ == 1) pageCharStart_ = parCharBase_;
    pageY_ = static_cast<int16_t>(pageY_ + h + params_.baseSizePx / 2);
  }

  // --- measure phase ----------------------------------------------------------

  // Decodes the codepoint at i (advancing i), applying Arabic contextual
  // forms and folding ligature pairs when the font substitutes one and both
  // halves share the span. Used by every measure/place/copy walk so widths
  // and baked run text always agree.
  uint32_t decodeShaped(uint32_t end, uint32_t& i, const Span& span) const {
    const uint32_t pos = i;
    uint32_t cp = decodeUtf8(parText_, end, i);
    uint8_t form = static_cast<uint8_t>(levels_[pos] >> kFormShift);
    if (form == kFormSkip) {
      // An alef whose lam sits outside this walk's bounds (pathological
      // split): render it joined to that lam.
      form = kFormFin;
    }
    if (form == kFormLamAlefIso || form == kFormLamAlefFin) {
      // shapeArabic only marks the pair when the font covers the ligature,
      // and the alef follows immediately; consume it.
      uint32_t lig = 0;
      if (i < end) {
        uint32_t j = i;
        const ArabLamAlef* la = lamAlefFor(decodeUtf8(parText_, end, j));
        if (la != nullptr) {
          lig = form == kFormLamAlefFin ? la->final : la->isolated;
          i = j;
        }
      }
      if (lig != 0) {
        cp = lig;
      } else {
        // The alef is beyond `end`: shape the lam alone, still joining it.
        const uint32_t solo =
            arabPresentation(cp, form == kFormLamAlefFin ? kFormMed : kFormIni);
        if (solo != 0 && params_.font->covers(solo)) cp = solo;
      }
    } else if (form != kFormNone) {
      const uint32_t shaped = arabPresentation(cp, form);
      if (shaped != 0 && params_.font->covers(shaped)) cp = shaped;
    }
    while (i < end && &spanAt(i) == &span) {
      if (params_.focusReading && ((levels_[pos] ^ levels_[i]) & kFocusBold)) break;
      uint32_t j = i;
      const uint32_t next = decodeUtf8(parText_, end, j);
      const uint32_t lig = params_.font->ligature(cp, next, span.flags);
      if (lig == 0) break;
      cp = lig;
      i = j;
    }
    return cp;
  }

  // The focus-reading bold prefix rides in the levels array; every
  // measurement site resolves its char's effective flags through this.
  uint8_t focusExtra(uint32_t pos) const {
    return params_.focusReading && (levels_[pos] & kFocusBold) ? StyleBold : StyleNone;
  }

  int32_t advanceFor(uint32_t cp, uint32_t prevCp, const Span& span,
                     uint8_t extraFlags = 0) const {
    if (cp == '\n' || cp == 0xAD) return 0;  // soft hyphen: invisible until a break uses it
    const uint16_t sizePx = spanSizePx(span);
    const uint8_t flags = static_cast<uint8_t>(span.flags | extraFlags);
    int32_t adv = params_.font->advance(cp, sizePx, flags);
    if (prevCp != 0) {
      adv += params_.font->kerning(prevCp, cp, sizePx, flags);
      if (crossesScripts(prevCp, cp)) adv += sizePx / 4;
      adv += cjkCompression(prevCp, cp, sizePx);
    }
    return adv;
  }

  // Width of parText_[from..to) with kerning, resetting kerning at `from`.
  int32_t measureRange(uint32_t from, uint32_t to) const {
    int32_t width = 0;
    uint32_t prev = 0;
    uint32_t i = from;
    while (i < to) {
      const uint32_t charStart = i;
      const Span& span = spanAt(charStart);
      if (charStart == span.start && span.marginBeforeRootPct > 0) {
        width += marginBeforePx(span);
        prev = 0;
      }
      const uint32_t cp = decodeShaped(to, i, span);
      width += advanceFor(cp, prev, span, focusExtra(charStart));
      prev = cp;
    }
    return width;
  }

  // Attempts to hyphenate the word starting at `wordStart` so that a prefix
  // (plus a hyphen) fits in `budget` measured from `lineStart`. Returns the
  // byte split position, or 0 if hyphenation does not help.
  uint32_t tryHyphenBreak(uint32_t lineStart, uint32_t wordStart, int32_t budget) {
    if (params_.hyphenator == nullptr || !params_.hyphenator->ready()) return 0;
    uint32_t wordEnd = wordStart;
    uint32_t wordChars = 0;
    while (wordEnd < parLen_) {
      uint32_t next = wordEnd;
      const uint32_t cp = decodeUtf8(parText_, parLen_, next);
      if (!isHyphLetter(cp)) break;
      wordEnd = next;
      ++wordChars;
    }
    const uint32_t wordLen = wordEnd - wordStart;
    if (wordChars < 5) return 0;

    uint8_t positions[16];
    const uint8_t n = params_.hyphenator->breakPositions(parText_ + wordStart, wordLen,
                                                         positions, sizeof(positions));
    if (n == 0) return 0;

    const int32_t prefixWidth = measureRange(lineStart, wordStart);
    const Span& span = spanAt(wordStart);
    const int32_t hyphenWidth =
        params_.font->advance('-', spanSizePx(span), span.flags);
    for (int i = n - 1; i >= 0; --i) {
      const uint32_t split = wordStart + positions[i];
      if (split <= lineStart) continue;
      const int32_t width = prefixWidth + measureRange(wordStart, split) + hyphenWidth;
      if (width <= budget) return split;
    }
    return 0;
  }

  void measureParagraphLines() {
    lineCount_ = 0;
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(parText_), parLen_, params_.language,
                        breaks_);
    bool hasArabic = false;
    paraRtl_ = computeBidiLevels(parText_, parLen_, levels_, &hasArabic);
    if (hasArabic) shapeArabic(parText_, parLen_, levels_, params_.font);
    if (params_.focusReading) markFocusWords(parText_, parLen_, levels_);

    // "ko-keep-all": word-unit breaking (CSS word-break: keep-all). UAX #14
    // allows breaks between Hangul syllables; this style demotes them so
    // Korean lines break only at spaces (libunibreak still sees "ko" for its
    // own tailoring — same suffix trick as "ja-strict").
    if (params_.language != nullptr) {
      const size_t langLen = strlen(params_.language);
      if (langLen >= 9 && strcmp(params_.language + langLen - 9, "-keep-all") == 0) {
        uint32_t i = 0;
        uint32_t prevCp = 0;
        uint32_t prevEnd = 0;
        while (i < parLen_) {
          const uint32_t cp = decodeUtf8(parText_, parLen_, i);
          if (prevEnd > 0 && isHangul(prevCp) && isHangul(cp) &&
              breaks_[prevEnd - 1] == LINEBREAK_ALLOWBREAK) {
            breaks_[prevEnd - 1] = LINEBREAK_NOBREAK;
          }
          prevCp = cp;
          prevEnd = i;
        }
      }
    }

    const int32_t baseMaxWidth =
        params_.pageWidth - params_.marginLeft - params_.marginRight - para_.indentPx;
    const uint16_t sizePx = paragraphSizePx();
    const int32_t textIndentPx =
        static_cast<int32_t>(sizePx) * (para_.textIndentPct > 0 ? para_.textIndentPct : 0) / 100;

    uint32_t lineStart = 0;
    uint32_t i = 0;
    int32_t lineWidth = 0;
    uint32_t prevCp = 0;
    uint32_t lastBreakEnd = 0;
    uint32_t wordStart = 0;

    while (i < parLen_ && lineCount_ + 1 < kMaxLinesPerPar) {
      const int32_t maxWidth = baseMaxWidth - (lineCount_ == 0 && !continued_ ? textIndentPx : 0);
      const uint32_t charStart = i;
      const Span& shapeSpan = spanAt(charStart);
      const uint32_t cp = decodeShaped(parLen_, i, shapeSpan);
      const int32_t leadingMargin =
          charStart == shapeSpan.start ? marginBeforePx(shapeSpan) : 0;
      const int32_t adv =
          leadingMargin + advanceFor(cp, leadingMargin > 0 ? 0 : prevCp, shapeSpan,
                                     focusExtra(charStart));

      if (cp != '\n' && lineWidth + adv > maxWidth && charStart > lineStart) {
        // Try a hyphen inside the overflowing word first; it beats breaking
        // at the previous space when it fits meaningfully more text.
        uint32_t breakEnd = 0;
        uint8_t flags = 0;
        const uint32_t hyphenAt =
            tryHyphenBreak(lineStart, wordStart > lineStart ? wordStart : lineStart, maxWidth);
        if (hyphenAt > lineStart &&
            (lastBreakEnd <= lineStart || hyphenAt > lastBreakEnd)) {
          breakEnd = hyphenAt;
          flags = kLineHyphen;
        } else if (lastBreakEnd > lineStart) {
          breakEnd = lastBreakEnd;
          if (lastBreakShy_) flags = kLineHyphen;
        } else {
          breakEnd = charStart;  // force mid-word, no opportunity at all
        }
        recordLine(lineStart, breakEnd, flags);
        lineStart = breakEnd;
        while (lineStart < parLen_ && parText_[lineStart] == ' ') ++lineStart;
        i = lineStart;
        lineWidth = 0;
        prevCp = 0;
        lastBreakEnd = 0;
        wordStart = lineStart;
        continue;
      }
      lineWidth += adv;
      prevCp = cp;

      const char brk = breaks_[i - 1];  // opportunity after this character
      if (brk == LINEBREAK_MUSTBREAK) {
        recordLine(lineStart, i, kLineLast);
        lineStart = i;
        lineWidth = 0;
        prevCp = 0;
        lastBreakEnd = 0;
        wordStart = i;
      } else if (brk == LINEBREAK_ALLOWBREAK) {
        lastBreakEnd = i;
        lastBreakShy_ = cp == 0xAD;  // breaking here must render a hyphen
        wordStart = i;
        while (wordStart < parLen_ && parText_[wordStart] == ' ') ++wordStart;
      }
    }
    if (lineStart < parLen_ && lineCount_ < kMaxLinesPerPar) {
      recordLine(lineStart, parLen_, kLineLast);
    }
    if (lineCount_ > 0) lines_[lineCount_ - 1].flags |= kLineLast;
  }

  void recordLine(uint32_t start, uint32_t end, uint8_t flags) {
    while (end > start && (parText_[end - 1] == ' ' || parText_[end - 1] == '\n')) --end;
    LineRec& rec = lines_[lineCount_++];
    rec.start = start;
    rec.end = end;
    rec.flags = flags;
    rec.maxSizePct = static_cast<uint8_t>(para_.sizePct > 255 ? 255 : para_.sizePct);
    rec.spaceCount = 0;
    rec.cjkGaps = 0;
    rec.hangulGaps = 0;
    uint32_t prev = 0;
    uint32_t i = start;
    while (i < end) {
      const uint32_t charStart = i;
      const uint32_t cp = decodeUtf8(parText_, end, i);
      const uint16_t charSizePct = spanAt(charStart).sizePct;
      if (charSizePct > rec.maxSizePct) {
        rec.maxSizePct = static_cast<uint8_t>(charSizePct > 255 ? 255 : charSizePct);
      }
      if (cp == ' ') {
        ++rec.spaceCount;
      } else if (prev != 0 && isCjk(prev) && isCjk(cp) &&
                 cjkCompression(prev, cp, 2) == 0) {  // compressed pairs don't stretch
        if (isHangul(prev) && isHangul(cp)) ++rec.hangulGaps;
        else ++rec.cjkGaps;
      }
      prev = cp;
    }
    rec.naturalWidth = measureRange(start, end);
    if (flags & kLineHyphen) {
      const Span& span = spanAt(end > start ? end - 1 : start);
      rec.naturalWidth += params_.font->advance('-', spanSizePx(span), span.flags);
    }
  }

  // --- place phase -------------------------------------------------------------

  void flushParagraph() {
    if (parLen_ == 0) {
      continued_ = false;
      return;
    }
    const uint16_t sizePx = paragraphSizePx();
    advanceY(static_cast<int16_t>(static_cast<int32_t>(sizePx) * para_.spaceBeforePct *
                                  params_.paragraphSpacingPct / 10000));

    measureParagraphLines();

    uint32_t idx = 0;
    while (idx < lineCount_ && !failed_ && !stopParse) {
      int32_t availPx = (params_.pageHeight - params_.marginBottom) - pageY_;
      uint32_t avail = 0;
      for (uint32_t probe = idx; probe < lineCount_ && availPx > 0; ++probe) {
        const int16_t lineHeight = lineHeightFor(sizePxForPct(lines_[probe].maxSizePct));
        if (lineHeight > availPx) break;
        availPx -= lineHeight;
        ++avail;
      }
      const uint32_t remaining = lineCount_ - idx;
      const bool hasContent = runCount_ > 0 || pageY_ > params_.marginTop;

      if (avail == 0) {
        if (!hasContent) break;  // page too short for even one line — give up
        emitPage();
        continue;
      }
      uint32_t take = avail < remaining ? avail : remaining;

      // Widow control first: never carry a runt to the next page, shrinking
      // this page's share if needed. Then orphan control on the result: if
      // the shrunken share would leave a runt at this page's bottom, push the
      // whole paragraph to the next page instead. (A 3-line paragraph with
      // 2 lines of room goes 0+3, not 2+1 or 1+2.)
      while (take < remaining && remaining - take < params_.widowLines && take > 1) --take;
      if (idx == 0 && take < remaining && take < params_.orphanLines && hasContent) {
        emitPage();
        continue;
      }

      for (uint32_t l = 0; l < take && !failed_ && !stopParse; ++l) {
        const uint16_t lineBoxSizePx = sizePxForPct(lines_[idx + l].maxSizePct);
        placeLine(lines_[idx + l], sizePx, lineBoxSizePx, lineHeightFor(lineBoxSizePx),
                  idx + l == 0);
      }
      idx += take;
      if (idx < lineCount_ && !stopParse) emitPage();
    }

    advanceY(static_cast<int16_t>(static_cast<int32_t>(sizePx) * para_.spaceAfterPct *
                                  params_.paragraphSpacingPct / 10000));
    parCharBase_ += countChars(parText_, parLen_);
    parLen_ = 0;
    spanCount_ = 0;
    pendingMarginRootPct_ = 0;
    pendingSpace_ = false;
    continued_ = false;
  }

  // One visual segment of a line: same span, same bidi level, no interior
  // adjustable gap. Collected logically, then reordered per UAX #9 L2.
  struct Seg {
    uint32_t start;
    uint32_t end;
    const Span* span;
    uint8_t level;
    bool gapBefore;       // an adjustable gap (space/CJK boundary) precedes it
    bool spaceBefore;     // that gap includes a real space advance
    bool scriptGap;       // quarter-em CJK/Latin air precedes it
    bool compressBefore;  // punctuation pair: half an em comes OUT before it
    uint8_t extraFlags;   // style added beyond the span's (focus-reading bold)
  };

  void placeLine(const LineRec& rec, uint16_t sizePx, uint16_t lineBoxSizePx,
                 int16_t lineHeight, bool firstLine) {
    if (rec.end == rec.start) {  // blank line (e.g. double <br/>)
      pageY_ += lineHeight;
      return;
    }
    if (runCount_ == 0) {
      pageCharStart_ = parCharBase_ + countChars(parText_, rec.start);
    }

    const int32_t baseMaxWidth =
        params_.pageWidth - params_.marginLeft - params_.marginRight - para_.indentPx;
    const int32_t textIndentPx =
        firstLine && !continued_ && para_.textIndentPct > 0
            ? static_cast<int32_t>(sizePx) * para_.textIndentPct / 100
            : 0;
    const int32_t maxWidth = baseMaxWidth - textIndentPx;
    const int32_t leftover = maxWidth - rec.naturalWidth;

    // Korean justifies at its word spaces; Hangul inter-syllable gaps only
    // stretch when the line has no spaces at all (space-less runs, or CJ).
    const bool hangulStretch = rec.spaceCount == 0;
    const uint32_t gaps = static_cast<uint32_t>(rec.spaceCount) + rec.cjkGaps +
                          (hangulStretch ? rec.hangulGaps : 0);
    const bool justify = para_.align == TextAlign::Justify && !(rec.flags & kLineLast) &&
                         gaps > 0 && leftover > 0;
    // RTL paragraphs mirror the alignment semantics: default/Left reads as
    // the start edge, which is the RIGHT edge for a Hebrew paragraph.
    TextAlign align = para_.align;
    if (paraRtl_) {
      if (align == TextAlign::Left) align = TextAlign::Right;
      else if (align == TextAlign::Right) align = TextAlign::Left;
    }
    int32_t x = params_.marginLeft + para_.indentPx +
                (paraRtl_ ? 0 : textIndentPx);
    if (align == TextAlign::Right) {
      x += leftover + (paraRtl_ ? 0 : 0);
    } else if (align == TextAlign::Center) {
      x += leftover / 2;
    }
    const int32_t perGap = justify ? leftover / static_cast<int32_t>(gaps) : 0;
    int32_t gapRemainder = justify ? leftover % static_cast<int32_t>(gaps) : 0;

    int16_t baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(lineBoxSizePx));

    // --- collect segments in LOGICAL order -----------------------------------
    Seg segs[64];
    uint32_t segCount = 0;
    {
      uint32_t segStart = rec.start;
      const Span* segSpan = &spanAt(rec.start);
      uint8_t segLevel = levels_[rec.start] & kBidiLevelMask;
      uint8_t segExtra = focusExtra(rec.start);
      bool gapBefore = false, spaceBefore = false, scriptGap = false, compressBefore = false;
      uint32_t linePrev = 0;
      uint32_t i = rec.start;
      auto close = [&](uint32_t endPos, bool nextGap, bool nextSpace, bool nextScript,
                       bool nextCompress) {
        if (endPos > segStart && segCount < 64) {
          segs[segCount++] = {segStart, endPos, segSpan, segLevel,
                              gapBefore, spaceBefore, scriptGap, compressBefore, segExtra};
        }
        gapBefore = nextGap;
        spaceBefore = nextSpace;
        scriptGap = nextScript;
        compressBefore = nextCompress;
      };
      while (i < rec.end) {
        const uint32_t charStart = i;
        const Span* span = &spanAt(charStart);
        const uint8_t level = levels_[charStart] & kBidiLevelMask;
        const uint8_t extra = focusExtra(charStart);
        uint32_t next = charStart;
        const uint32_t cp = decodeShaped(rec.end, next, *span);

        if (justify && cp == ' ') {
          close(charStart, true, true, false, false);
          segStart = next;
          segSpan = &spanAt(next < rec.end ? next : charStart);
          segLevel = next < rec.end ? (levels_[next] & kBidiLevelMask) : level;
          segExtra = next < rec.end ? focusExtra(next) : extra;
          linePrev = cp;
          i = next;
          continue;
        }
        const bool compGap = linePrev != 0 && cjkCompression(linePrev, cp, 2) != 0;
        const bool cjkGap = linePrev != 0 && !compGap && justify && isCjk(linePrev) &&
                            isCjk(cp) &&
                            (hangulStretch || !(isHangul(linePrev) && isHangul(cp)));
        const bool mixGap = linePrev != 0 && crossesScripts(linePrev, cp);
        if (span != segSpan || level != segLevel || extra != segExtra || cjkGap || mixGap ||
            compGap) {
          close(charStart, cjkGap, false, mixGap, compGap);
          segStart = charStart;
          segSpan = span;
          segLevel = level;
          segExtra = extra;
        }
        linePrev = cp;
        i = next;
      }
      close(rec.end, false, false, false, false);
    }
    if (segCount == 0) {
      pageY_ += lineHeight;
      return;
    }

    // --- reorder to VISUAL order (UAX #9 L2) ---------------------------------
    uint8_t order[64];
    for (uint32_t s = 0; s < segCount; ++s) order[s] = static_cast<uint8_t>(s);
    uint8_t maxLevel = 0, minOdd = 255;
    for (uint32_t s = 0; s < segCount; ++s) {
      if (segs[s].level > maxLevel) maxLevel = segs[s].level;
      if ((segs[s].level & 1) && segs[s].level < minOdd) minOdd = segs[s].level;
    }
    const uint8_t base = paraRtl_ ? 1 : 0;
    for (uint8_t lvl = maxLevel; lvl >= (minOdd == 255 ? 1 : minOdd) && lvl >= base + 1u &&
                                 lvl != 0; --lvl) {
      uint32_t s = 0;
      while (s < segCount) {
        if (segs[order[s]].level < lvl) { ++s; continue; }
        uint32_t e = s;
        while (e < segCount && segs[order[e]].level >= lvl) ++e;
        for (uint32_t a = s, b = e - 1; a < b; ++a, --b) {
          const uint8_t t = order[a]; order[a] = order[b]; order[b] = t;
        }
        s = e;
      }
    }
    if (paraRtl_) {  // base level 1: reverse everything once more (L2 for lvl 1)
      for (uint32_t a = 0, b = segCount - 1; a < b; ++a, --b) {
        const uint8_t t = order[a]; order[a] = order[b]; order[b] = t;
      }
      // RTL first-line indent comes off the right edge: shift line body left.
      // (x already excludes textIndent; the line simply starts at the margin.)
    }

    // --- emit in visual order -------------------------------------------------
    for (uint32_t v = 0; v < segCount; ++v) {
      const Seg& sg = segs[order[v]];
      // Gap handling follows VISUAL adjacency: apply the logical gap cost of
      // the segment it precedes (space advance + justify stretch).
      if (v > 0) {
        const Seg& logicalOwner = sg;
        if (logicalOwner.gapBefore || logicalOwner.spaceBefore) {
          if (logicalOwner.spaceBefore) {
            x += params_.font->advance(' ', spanSizePx(*logicalOwner.span),
                                       logicalOwner.span->flags);
          }
          if (justify) {
            x += perGap;
            if (gapRemainder > 0) { ++x; --gapRemainder; }
          }
        } else if (logicalOwner.scriptGap) {
          x += spanSizePx(*logicalOwner.span) / 4;
        } else if (logicalOwner.compressBefore) {
          x -= spanSizePx(*logicalOwner.span) / 2;  // punctuation pair overlap
        }
      }
      if (sg.start == sg.span->start && sg.span->marginBeforeRootPct > 0) {
        x += marginBeforePx(*sg.span);
      }
      const bool lastLogical = sg.end == rec.end;
      const bool addHyphen = lastLogical && (rec.flags & kLineHyphen) != 0 && !paraRtl_;
      if (!emitSeg(sg, addHyphen, baselineY, x, sizePx)) return;
    }
    pageY_ += lineHeight;
  }

  // Emits one segment as a page run at x (advancing it). Splits pages when
  // run/arena capacity is hit. Returns false on stop/failure.
  bool emitSeg(const Seg& sg, bool addHyphen, int16_t& baselineY, int32_t& x,
               uint16_t sizePx) {
    // Measure the segment (kerning resets at its start, matching natural
    // width accounting at gap boundaries).
    int32_t segWidth = 0;
    {
      uint32_t i = sg.start;
      uint32_t prev = 0;
      while (i < sg.end) {
        const uint32_t cp = decodeShaped(sg.end, i, *sg.span);
        segWidth += advanceFor(cp, prev, *sg.span, sg.extraFlags);
        prev = cp;
      }
    }
    const uint8_t runFlags = static_cast<uint8_t>(sg.span->flags | sg.extraFlags);
    const uint32_t segLen = sg.end - sg.start;
    const uint32_t copyLen = segLen + segLen / 2 + (addHyphen ? 1u : 0u) + 4;
    if (runCount_ >= kMaxRunsPerPage) {
      emitPage();
      if (stopParse) return false;
      baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(sizePx));
    }
    char* copy = static_cast<char*>(pageArena_.alloc(copyLen, 1));
    if (copy == nullptr) {
      emitPage();
      if (stopParse) return false;
      baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(sizePx));
      copy = static_cast<char*>(pageArena_.alloc(copyLen, 1));
      if (copy == nullptr) {
        failed_ = true;
        return false;
      }
    }
    // Re-encode: ligatures bake in, soft hyphens drop, RTL runs mirror
    // brackets (UAX #9 L4).
    uint32_t outLen = 0;
    uint32_t ci = sg.start;
    while (ci < sg.end) {
      uint32_t cp2 = decodeShaped(sg.end, ci, *sg.span);
      if (cp2 == 0xAD) continue;
      if (sg.level & 1) cp2 = bidiMirror(cp2);
      outLen += encodeUtf8(cp2, copy + outLen);
    }
    if (sg.level & 1) reverseUtf8(copy, outLen);  // store visual order (L2)
    if (addHyphen) {
      copy[outLen++] = '-';
      segWidth += params_.font->advance('-', spanSizePx(*sg.span), runFlags);
    }
    int16_t runBaseline = baselineY;
    if (sg.span->flags & StyleSuperscript) {
      runBaseline = static_cast<int16_t>(baselineY - (spanSizePx(*sg.span) * 33) / 100);
    } else if (sg.span->flags & StyleSubscript) {
      runBaseline = static_cast<int16_t>(baselineY + (spanSizePx(*sg.span) * 12) / 100);
    }
    runs_[runCount_++] = {copy,
                          static_cast<uint16_t>(outLen),
                          static_cast<int16_t>(x),
                          runBaseline,
                          spanSizePx(*sg.span),
                          runFlags};
    if (sg.span->link != 0 && linkCount_ < kMaxLinksPerPage) {
      const uint8_t li = static_cast<uint8_t>(sg.span->link - 1);
      const uint16_t sz = spanSizePx(*sg.span);
      PageLink& pl = links_[linkCount_++];
      pl.target = pageArena_.strdup(parLinkPath_[li]);
      pl.fragment = pageArena_.strdup(parLinkFrag_[li]);
      pl.x = static_cast<int16_t>(x);
      pl.y = static_cast<int16_t>(runBaseline - sz);
      pl.width = static_cast<uint16_t>(segWidth > 0 ? segWidth : 1);
      pl.height = static_cast<uint16_t>(sz + sz / 4);
      if (pl.target == nullptr || pl.fragment == nullptr) --linkCount_;
    }
    x += segWidth;
    return true;
  }

  void emitPage() {
    Page page{runs_, runCount_, images_, imageCount_, links_, linkCount_, pageCount_,
              pageCharStart_};
    ++pageCount_;
    if (!sink_.onPage(page)) stopParse = true;
    runCount_ = 0;
    imageCount_ = 0;
    linkCount_ = 0;
    pageArena_.reset();
    pageY_ = params_.marginTop;
  }

  BookSource& source_;
  const ZipCatalog* zip_;  // nullptr for plain-text layout (no container)
  const LayoutParams& params_;
  Arena& scratch_;
  Arena& parseArena_;  // inflate/XML state (== scratch_ unless the caller split)
  PageSink& sink_;
  char chapterDir_[512];

  char* parText_ = nullptr;
  char* breaks_ = nullptr;
  uint8_t* levels_ = nullptr;  // bidi embedding level per byte
  bool paraRtl_ = false;
  Span* spans_ = nullptr;
  PageTextRun* runs_ = nullptr;
  PageImage* images_ = nullptr;
  LineRec* lines_ = nullptr;
  Arena pageArena_;

  uint32_t parLen_ = 0;
  uint16_t spanCount_ = 0;
  uint32_t pendingMarginRootPct_ = 0;
  uint32_t lineCount_ = 0;
  ParaStyle para_{100, StyleNone, TextAlign::Left, 0, 0, 0, 40};
  bool pendingSpace_ = false;
  bool continued_ = false;

  ElemState stack_[kMaxElemDepth];
  uint8_t stackTop_ = 0;
  int stackOverflow_ = 0;
  int depth_ = 0;
  int suppress_ = 0;
  bool inBody_ = false;

  uint16_t runCount_ = 0;
  uint16_t imageCount_ = 0;
  int16_t pageY_ = 0;
  uint32_t pageCount_ = 0;
  char* styleText_ = nullptr;
  uint32_t styleLen_ = 0;
  int inStyle_ = 0;
  bool chapterCssOk_ = false;
  CssStylesheetBuilder chapterBuilder_;
  CssStylesheet chapterSheet_{};
  char parLinkPath_[kMaxLinksPerPar][160];
  char parLinkFrag_[kMaxLinksPerPar][48];
  uint8_t parLinkCount_ = 0;
  uint8_t currentLink_ = 0;
  PageLink* links_ = nullptr;
  uint16_t linkCount_ = 0;
  const ProbedImage* probed_ = nullptr;  // pre-scanned image dimensions
  uint16_t probedCount_ = 0;
  bool lastBreakShy_ = false;   // last ALLOWBREAK sat on a soft hyphen
  uint32_t parCharBase_ = 0;    // chapter chars before the current paragraph
  uint32_t pageCharStart_ = 0;  // anchor of the page being assembled
  bool failed_ = false;
};

}  // namespace

namespace {

// Image pre-scan: gather image hrefs in a collector parse (reading the
// chapter from `chapterSource`), then probe each target's dimensions in the
// BOOK container with only one inflate stream alive at a time. The table
// sits below the layout buffers; everything else the pre-scan used is
// released before the main parse, so the chapter peak stays at the
// text-chapter level. Failures here are non-fatal — the main parse reports
// real errors, and unprobed images fall back to the inline probe.
void prescanImages(BookSource& bookSource, const ZipCatalog* zip, BookSource& chapterSource,
                   const ZipEntry& entry, const char* chapterHref, Arena& scratch,
                   Arena& parseArena, ProbedImage** probedOut, uint16_t* probedCountOut) {
  *probedOut = nullptr;
  *probedCountOut = 0;
  if (zip == nullptr) return;
  const size_t tableMark = scratch.mark();
  ProbedImage* probed = nullptr;
  uint16_t probedCount = 0;
  if (entryMentionsImages(chapterSource, entry, parseArena)) {
    probed = scratch.allocArray<ProbedImage>(kMaxProbedImages);
    if (probed != nullptr) {
      ImageCollector collector(chapterHref, scratch, probed, kMaxProbedImages);
      if (XmlSax::parseEntry(chapterSource, entry, parseArena, collector,
                             /*filterHtmlEntities=*/true) == BookStatus::Ok ||
          collector.count() > 0) {
        probedCount = collector.count();
        // Resolve each collected href through the catalog's hash lookup (the
        // same name hash find() keys on) -- virtual, so an SD-backed catalog
        // serves this without an in-RAM entry table. A missing target keeps
        // kind=Unknown and layout skips the image, exactly as the inline
        // probe path did.
        for (uint16_t i = 0; i < probedCount; ++i) {
          const ZipEntry* target = zip->findByHash(probed[i].hrefHash);
          if (target != nullptr) probeImage(bookSource, *target, parseArena, &probed[i].info);
        }
      }
    }
    if (probedCount == 0) {
      scratch.release(tableMark);  // reclaim the unused table
      probed = nullptr;
    }
  }
  // No parseArena release here: every helper above self-releases its own
  // allocations, and in single-arena mode a release to a pre-table mark
  // would clobber the probed table (parseArena aliases scratch then).
  *probedOut = probed;
  *probedCountOut = probedCount;
}

}  // namespace

// --- ChapterLayoutSession ----------------------------------------------------

class ChapterLayoutSession::CountingSink : public PageSink {
 public:
  explicit CountingSink(PageSink& inner) : inner_(inner) {}
  void onAnchor(uint32_t idHash, uint32_t charStart) override {
    inner_.onAnchor(idHash, charStart);
  }
  bool onPage(const Page& page) override {
    ++pages_;
    return inner_.onPage(page);
  }
  uint32_t pages() const { return pages_; }

 private:
  PageSink& inner_;
  uint32_t pages_ = 0;
};

BookStatus ChapterLayoutSession::begin(BookSource& bookSource, const ZipCatalog* zip,
                                       BookSource& chapterSource, const ZipEntry& entry,
                                       const char* chapterHref, const LayoutParams& params,
                                       Arena& scratch, PageSink& sink, Arena* parseScratch,
                                       Arena* prescanScratch) {
  abort();
  if (params.font == nullptr || params.pageWidth <= 0 || params.pageHeight <= 0) {
    return BookStatus::Unsupported;
  }
  Arena& parseArena = parseScratch != nullptr ? *parseScratch : scratch;
  Arena& prescanArena = prescanScratch != nullptr ? *prescanScratch : parseArena;
  bytesTotal_ = entry.uncompressedSize;

  ProbedImage* probed = nullptr;
  uint16_t probedCount = 0;
  prescanImages(bookSource, zip, chapterSource, entry, chapterHref, scratch, prescanArena, &probed,
                &probedCount);

  auto* counting =
      static_cast<CountingSink*>(scratch.alloc(sizeof(CountingSink), alignof(CountingSink)));
  auto* engine =
      static_cast<LayoutEngine*>(scratch.alloc(sizeof(LayoutEngine), alignof(LayoutEngine)));
  if (counting == nullptr || engine == nullptr) return BookStatus::OutOfMemory;
  counting = new (counting) CountingSink(sink);
  engine = new (engine) LayoutEngine(bookSource, zip, chapterHref, params, scratch, *counting,
                                     probed, probedCount, &parseArena);
  countingSink_ = counting;
  engine_ = engine;
  if (!engine->init()) {
    state_ = State::Failed;
    return BookStatus::OutOfMemory;
  }

  const BookStatus st = sax_.open(chapterSource, entry, parseArena, *engine,
                                  /*filterHtmlEntities=*/true);
  if (st != BookStatus::Ok) {
    state_ = State::Failed;
    return st;
  }
  state_ = State::Parsing;
  return BookStatus::Ok;
}

BookStatus ChapterLayoutSession::step(uint32_t minNewPages) {
  if (state_ == State::Done) return BookStatus::Ok;
  if (state_ != State::Parsing) return BookStatus::Unsupported;
  auto* engine = static_cast<LayoutEngine*>(engine_);
  auto* counting = static_cast<CountingSink*>(countingSink_);

  const uint32_t startPages = counting->pages();
  bool atEnd = false;
  BookStatus status = BookStatus::Ok;
  while (!atEnd && !engine->stopParse && counting->pages() - startPages < minNewPages) {
    status = sax_.feedChunk(&atEnd);
    if (status != BookStatus::Ok) break;
  }
  if (status == BookStatus::Ok && (atEnd || engine->stopParse)) {
    if (!engine->finish()) status = BookStatus::OutOfMemory;
    if (status == BookStatus::Ok && engine->outOfMemory()) status = BookStatus::OutOfMemory;
    sax_.close();
    state_ = status == BookStatus::Ok ? State::Done : State::Failed;
    return status;
  }
  if (status != BookStatus::Ok) {
    if (status != BookStatus::OutOfMemory && engine->outOfMemory()) {
      status = BookStatus::OutOfMemory;
    }
    sax_.close();
    state_ = State::Failed;
  }
  return status;
}

uint32_t ChapterLayoutSession::pagesEmitted() const {
  return countingSink_ != nullptr ? static_cast<const CountingSink*>(countingSink_)->pages() : 0;
}

uint32_t ChapterLayoutSession::totalChars() const {
  return engine_ != nullptr ? static_cast<const LayoutEngine*>(engine_)->totalChars() : 0;
}

void ChapterLayoutSession::abort() {
  sax_.close();
  if (engine_ != nullptr) {
    static_cast<LayoutEngine*>(engine_)->~LayoutEngine();
    engine_ = nullptr;
  }
  if (countingSink_ != nullptr) {
    static_cast<CountingSink*>(countingSink_)->~CountingSink();
    countingSink_ = nullptr;
  }
  bytesTotal_ = 0;
  state_ = State::Idle;
}

BookStatus ChapterLayout::layout(BookSource& source, const ZipCatalog& zip,
                                 const ZipEntry& entry, const char* chapterHref,
                                 const LayoutParams& params, Arena& scratch, PageSink& sink,
                                 uint32_t* pageCountOut, uint32_t* totalCharsOut,
                                 Arena* parseScratch) {
  // One-shot = a session pumped to completion; a single code path keeps
  // stepped and blocking layouts byte-identical by construction.
  const size_t marked = scratch.mark();
  Arena& parseArena = parseScratch != nullptr ? *parseScratch : scratch;
  const size_t parseMarked = parseArena.mark();

  ChapterLayoutSession session;
  BookStatus status =
      session.begin(source, &zip, source, entry, chapterHref, params, scratch, sink, parseScratch);
  while (status == BookStatus::Ok && !session.done()) {
    status = session.step(UINT32_MAX);
  }
  if (pageCountOut != nullptr) *pageCountOut = session.pagesEmitted();
  if (totalCharsOut != nullptr) *totalCharsOut = session.totalChars();
  session.abort();
  scratch.release(marked);
  parseArena.release(parseMarked);
  return status;
}

BookStatus ChapterLayout::layoutPlainText(BookSource& source, const LayoutParams& params,
                                          Arena& scratch, PageSink& sink,
                                          uint32_t* pageCountOut, uint32_t* totalCharsOut) {
  if (params.font == nullptr || params.pageWidth <= 0 || params.pageHeight <= 0) {
    return BookStatus::Unsupported;
  }
  const size_t marked = scratch.mark();
  LayoutEngine engine(source, nullptr, "", params, scratch, sink);
  char* buf = static_cast<char*>(scratch.alloc(4096, 1));
  if (!engine.init() || buf == nullptr) {
    scratch.release(marked);
    return BookStatus::OutOfMemory;
  }

  // Plain text is "one chapter of <p> elements": paragraphs split on blank
  // lines, single newlines flow as spaces (the engine's whitespace collapse
  // handles that). UTF-8 assumed; a leading BOM is skipped.
  engine.onStartElement("body", nullptr);
  engine.onStartElement("p", nullptr);
  uint64_t offset = 0;
  uint32_t newlines = 0;
  bool first = true;
  BookStatus status = BookStatus::Ok;
  for (;;) {
    const int32_t n = source.readAt(offset, buf, 4096);
    if (n < 0) {
      status = BookStatus::IoError;
      break;
    }
    if (n == 0) break;
    offset += static_cast<uint32_t>(n);
    int32_t start = 0;
    if (first) {
      first = false;
      if (n >= 3 && static_cast<uint8_t>(buf[0]) == 0xEF &&
          static_cast<uint8_t>(buf[1]) == 0xBB && static_cast<uint8_t>(buf[2]) == 0xBF) {
        start = 3;  // UTF-8 BOM
      }
    }
    int32_t runStart = start;
    for (int32_t i = start; i < n; ++i) {
      const char c = buf[i];
      if (c == '\n') {
        ++newlines;
        if (newlines >= 2) {  // blank line = paragraph break
          if (i > runStart) engine.onText(buf + runStart, i - runStart);
          engine.onEndElement("p");
          engine.onStartElement("p", nullptr);
          runStart = i + 1;
          newlines = 0;
        }
      } else if (c != '\r') {
        newlines = 0;
      }
    }
    if (n > runStart) engine.onText(buf + runStart, n - runStart);
    if (engine.stopParse) break;
  }
  engine.onEndElement("p");
  engine.onEndElement("body");
  if (status == BookStatus::Ok && !engine.finish()) status = BookStatus::OutOfMemory;
  if (pageCountOut != nullptr) *pageCountOut = engine.pageCount();
  if (totalCharsOut != nullptr) *totalCharsOut = engine.totalChars();
  scratch.release(marked);
  return status;
}

}  // namespace book
}  // namespace freeink
