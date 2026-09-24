#include "Gpos.h"

#include <string.h>

namespace freeink {
namespace font {
namespace gpos {

namespace {

constexpr uint32_t kTagLatn = uint32_t('l') << 24 | uint32_t('a') << 16 | uint32_t('t') << 8 | 'n';
constexpr uint32_t kTagDflt = uint32_t('D') << 24 | uint32_t('F') << 16 | uint32_t('L') << 8 | 'T';

struct View {
  const uint8_t* data = nullptr;
  size_t size = 0;

  bool has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  unsigned u16(size_t offset) const { return has(offset, 2) ? unsigned(data[offset]) * 256 + data[offset + 1] : 0; }
  int32_t s16(size_t offset) const { return int32_t(int16_t(u16(offset))); }
  uint32_t u32(size_t offset) const { return has(offset, 4) ? (uint32_t(u16(offset)) << 16) | u16(offset + 2) : 0; }
  // Offset 0 is OpenType's own NULL convention for an absent optional
  // field — treating it as a real offset would alias `this` view's own
  // header as the child table (harmless memory-safety-wise since it's still
  // in bounds, but can manufacture a bogus "match" out of a malformed font).
  View sub(size_t offset) const {
    return (offset != 0 && has(offset, 1)) ? View{data + offset, size - offset} : View{};
  }
};

// CoverageTable lookup (identical layout to GSUB's): returns the coverage
// index of `glyph`, or -1. Format 2's linear scan keeps its own cap for the
// same reason as Gsub.cpp's: it isn't metered by the outer `work` budget.
int coverageIndex(View view, unsigned glyph) {
  const unsigned format = view.u16(0);
  const unsigned rawCount = view.u16(2);
  const unsigned count = rawCount < 8192 ? rawCount : 8192;
  if (format == 1 && view.has(4, size_t(count) * 2)) {
    unsigned low = 0;
    unsigned high = count;
    while (low < high) {
      const unsigned middle = (low + high) / 2;
      if (view.u16(4 + middle * 2) < glyph)
        low = middle + 1;
      else
        high = middle;
    }
    return low < count && view.u16(4 + low * 2) == glyph ? int(low) : -1;
  }
  if (format == 2 && view.has(4, size_t(count) * 6)) {
    for (unsigned i = 0; i < count; ++i) {
      const size_t position = 4 + i * 6;
      const unsigned start = view.u16(position);
      const unsigned end = view.u16(position + 2);
      if (glyph >= start && glyph <= end) return int(view.u16(position + 4) + glyph - start);
    }
  }
  return -1;
}

// ClassDefTable lookup: the glyph's class, with 0 (the spec's catch-all
// class) for any glyph the table doesn't list. Same self-cap as coverage.
unsigned glyphClass(View view, unsigned glyph) {
  const unsigned format = view.u16(0);
  if (format == 1) {
    const unsigned start = view.u16(2);
    const unsigned rawCount = view.u16(4);
    const unsigned count = rawCount < 8192 ? rawCount : 8192;
    if (!view.has(6, size_t(count) * 2)) return 0;
    return (glyph >= start && glyph - start < count) ? view.u16(6 + (glyph - start) * 2) : 0;
  }
  if (format == 2) {
    const unsigned rawCount = view.u16(2);
    const unsigned count = rawCount < 8192 ? rawCount : 8192;
    if (!view.has(4, size_t(count) * 6)) return 0;
    for (unsigned i = 0; i < count; ++i) {
      const size_t position = 4 + i * 6;
      const unsigned start = view.u16(position);
      const unsigned end = view.u16(position + 2);
      if (glyph >= start && glyph <= end) return view.u16(position + 4);
    }
  }
  return 0;
}

// Every set bit in a ValueFormat contributes one uint16 to its ValueRecord
// (placements, advances, and device-table offsets alike).
unsigned valueRecordSize(unsigned valueFormat) {
  unsigned bits = valueFormat & 0xFFFFu;
  unsigned count = 0;
  while (bits) {
    count += bits & 1u;
    bits >>= 1;
  }
  return count * 2;
}

constexpr unsigned kXAdvanceBit = 0x0004;

// Byte offset of the XAdvance field inside a ValueRecord: one uint16 per set
// bit below it (XPlacement 0x0001, YPlacement 0x0002).
unsigned xAdvanceOffset(unsigned valueFormat) {
  unsigned offset = 0;
  if (valueFormat & 0x0001) offset += 2;
  if (valueFormat & 0x0002) offset += 2;
  return offset;
}

// A PairPos subtable's adjustment for (left, right). `found` distinguishes a
// genuine record (even a zero-valued one — format 2 matrices are dense, and a
// matched pair must stop the lookup walk exactly as a shaper's would) from
// "this subtable does not cover the pair".
int32_t pairAdjustment(View subtable, unsigned left, unsigned right, bool& found) {
  found = false;
  const unsigned format = subtable.u16(0);
  const unsigned valueFormat1 = subtable.u16(4);
  const unsigned valueFormat2 = subtable.u16(6);
  if ((valueFormat1 & kXAdvanceBit) == 0) return 0;  // no X-advance data → nothing to kern with
  const unsigned len1 = valueRecordSize(valueFormat1);
  const unsigned len2 = valueRecordSize(valueFormat2);
  const unsigned advanceAt = xAdvanceOffset(valueFormat1);

  if (format == 1) {
    const int coverage = coverageIndex(subtable.sub(subtable.u16(2)), left);
    const unsigned setCount = subtable.u16(8);
    if (coverage < 0 || unsigned(coverage) >= setCount || !subtable.has(10, size_t(setCount) * 2)) return 0;
    View set = subtable.sub(subtable.u16(10 + unsigned(coverage) * 2));
    const unsigned pairCount = set.u16(0);
    const size_t recordSize = 2 + size_t(len1) + len2;
    if (!set.has(2, size_t(pairCount) * recordSize)) return 0;
    // PairValueRecords are sorted by SecondGlyph.
    unsigned low = 0;
    unsigned high = pairCount;
    while (low < high) {
      const unsigned middle = (low + high) / 2;
      if (set.u16(2 + middle * recordSize) < right)
        low = middle + 1;
      else
        high = middle;
    }
    if (low >= pairCount || set.u16(2 + low * recordSize) != right) return 0;
    found = true;
    return set.s16(2 + low * recordSize + 2 + advanceAt);
  }

  if (format == 2) {
    // Coverage lists the first glyphs; a covered left glyph always resolves
    // to a (possibly zero) record through the class matrix.
    if (coverageIndex(subtable.sub(subtable.u16(2)), left) < 0) return 0;
    const unsigned class1Count = subtable.u16(12);
    const unsigned class2Count = subtable.u16(14);
    const unsigned class1 = glyphClass(subtable.sub(subtable.u16(8)), left);
    const unsigned class2 = glyphClass(subtable.sub(subtable.u16(10)), right);
    if (class1 >= class1Count || class2 >= class2Count) return 0;
    const size_t recordSize = size_t(len1) + len2;
    const size_t record = 16 + (size_t(class1) * class2Count + class2) * recordSize;
    if (!subtable.has(16, size_t(class1Count) * class2Count * recordSize)) return 0;
    found = true;
    return subtable.s16(record + advanceAt);
  }

  return 0;
}

}  // namespace

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph) {
  if (gposBytes == nullptr || leftGlyph == 0 || rightGlyph == 0) return 0;
  View gpos{gposBytes, gposSize};
  if (!gpos.has(0, 10)) return 0;
  View scripts = gpos.sub(gpos.u16(4));
  View features = gpos.sub(gpos.u16(6));
  View lookups = gpos.sub(gpos.u16(8));
  if (!scripts.has(0, 2)) return 0;
  const unsigned featureCount = features.u16(0);
  const unsigned lookupCount = lookups.u16(0);
  // Counts are 16-bit fields with no artificial low cap: `.has()` below
  // prevents out-of-bounds reads, while the selected-language walk and the
  // total `work` budget bound the expensive operations (same policy as
  // Gsub.cpp — see the rationale there).
  if (!features.has(2, size_t(featureCount) * 6) || !lookups.has(2, size_t(lookupCount) * 2)) return 0;

  // Script/feature activation policy is identical to Gsub.cpp's: only the
  // `latn` (fallback `DFLT`) DefaultLangSys may activate `kern`.
  auto usableLangSys = [](View langSys) {
    if (!langSys.has(0, 6)) return false;
    return langSys.has(6, size_t(langSys.u16(4)) * 2);
  };
  const unsigned scriptCount = scripts.u16(0);
  if (!scripts.has(2, size_t(scriptCount) * 6)) return 0;

  unsigned work = 0;
  auto findDefaultLangSys = [&](const uint32_t wanted) -> View {
    unsigned low = 0;
    unsigned high = scriptCount;
    while (low < high) {
      if (++work > 4096) return View{};
      const unsigned middle = low + (high - low) / 2;
      const size_t record = 2 + size_t(middle) * 6;
      const uint32_t tag = scripts.u32(record);
      if (tag < wanted) {
        low = middle + 1;
      } else {
        high = middle;
      }
    }
    if (low >= scriptCount) return View{};
    const size_t record = 2 + size_t(low) * 6;
    if (scripts.u32(record) != wanted) return View{};
    const View script = scripts.sub(scripts.u16(record + 4));
    return script.has(0, 4) ? script.sub(script.u16(0)) : View{};
  };

  const View latinLangSys = findDefaultLangSys(kTagLatn);
  const View dfltLangSys = findDefaultLangSys(kTagDflt);
  const View langSys = usableLangSys(latinLangSys) ? latinLangSys : dfltLangSys;
  if (!usableLangSys(langSys)) return 0;
  const unsigned requiredFeatureIndex = langSys.u16(2);
  const unsigned featureIndexCount = langSys.u16(4);
  if (!langSys.has(6, size_t(featureIndexCount) * 2)) return 0;

  auto processFeature = [&](unsigned featureIndex, bool& found) -> int32_t {
    found = false;
    if (featureIndex >= featureCount || ++work > 4096) return 0;
    const size_t record = 2 + featureIndex * 6;
    if (memcmp(features.data + record, "kern", 4) != 0) return 0;
    View feature = features.sub(features.u16(record + 4));
    const unsigned count = feature.u16(2);
    if (!feature.has(4, size_t(count) * 2)) return 0;
    for (unsigned i = 0; i < count; ++i) {
      if (++work > 4096) return 0;
      const unsigned index = feature.u16(4 + i * 2);
      if (index >= lookupCount) continue;
      View lookup = lookups.sub(lookups.u16(2 + index * 2));
      const unsigned type = lookup.u16(0);
      const unsigned subtableCount = lookup.u16(4);
      if ((type != 2 && type != 9) || !lookup.has(6, size_t(subtableCount) * 2)) continue;
      for (unsigned j = 0; j < subtableCount; ++j) {
        if (++work > 4096) return 0;
        View subtable = lookup.sub(lookup.u16(6 + j * 2));
        if (type == 9) {
          // Extension Positioning: format 1 wraps another lookup type at a
          // 32-bit offset. Unwrap it if it wraps a pair adjustment (2).
          if (!subtable.has(0, 8) || subtable.u16(0) != 1 || subtable.u16(2) != 2) continue;
          subtable = subtable.sub(subtable.u32(4));
        }
        const int32_t value = pairAdjustment(subtable, leftGlyph, rightGlyph, found);
        // A found record — zero-valued or not — ends the walk: within a
        // lookup only the first matching subtable applies, and this
        // single-feature reader treats the first matching lookup as final.
        if (found) return value;
      }
    }
    return 0;
  };

  bool found = false;
  if (requiredFeatureIndex != 0xFFFFu) {
    const int32_t result = processFeature(requiredFeatureIndex, found);
    if (found) return result;
  }
  for (unsigned i = 0; i < featureIndexCount; ++i) {
    const int32_t result = processFeature(langSys.u16(6 + i * 2), found);
    if (found) return result;
  }
  return 0;
}

}  // namespace gpos
}  // namespace font
}  // namespace freeink
