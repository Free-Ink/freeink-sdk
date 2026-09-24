#include "Gsub.h"

#include <string.h>

namespace freeink {
namespace font {
namespace gsub {

namespace {

constexpr uint32_t kTagLatn = uint32_t('l') << 24 | uint32_t('a') << 16 | uint32_t('t') << 8 | 'n';
constexpr uint32_t kTagDflt = uint32_t('D') << 24 | uint32_t('F') << 16 | uint32_t('L') << 8 | 'T';

struct View {
  const uint8_t* data = nullptr;
  size_t size = 0;

  bool has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  unsigned u16(size_t offset) const { return has(offset, 2) ? unsigned(data[offset]) * 256 + data[offset + 1] : 0; }
  uint32_t u32(size_t offset) const { return has(offset, 4) ? (uint32_t(u16(offset)) << 16) | u16(offset + 2) : 0; }
  // Offset 0 is OpenType's own NULL convention for an absent optional
  // field — treating it as a real offset would alias `this` view's own
  // header as the child table (harmless memory-safety-wise since it's still
  // in bounds, but can manufacture a bogus "match" out of a malformed font).
  View sub(size_t offset) const {
    return (offset != 0 && has(offset, 1)) ? View{data + offset, size - offset} : View{};
  }
};

// GSUB CoverageTable lookup: returns the coverage index of `glyph`, or -1.
// Format 1's search is a binary search (cheap even at the 16-bit field's
// natural max), but format 2's is linear, and coverageIndex() isn't reached
// through the outer loop's `work` budget below — so unlike every other count
// in this file, this one keeps its own cap. 8192 is far beyond any real
// font's coverage range-list size (coverage compacts into ranges, so even
// huge CJK fonts stay in the tens-to-low-hundreds), but still bounds
// worst-case scan time against a hostile "count" field.
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

}  // namespace

uint32_t LigatureGlyphId(const uint8_t* gsubBytes, size_t gsubSize, const uint32_t* sequence, unsigned length) {
  if (gsubBytes == nullptr || sequence == nullptr) return 0;
  View gsub{gsubBytes, gsubSize};
  if (!gsub.has(0, 10) || length < 2 || length > 3) return 0;
  View scripts = gsub.sub(gsub.u16(4));
  View features = gsub.sub(gsub.u16(6));
  View lookups = gsub.sub(gsub.u16(8));
  if (!scripts.has(0, 2)) return 0;
  const unsigned featureCount = features.u16(0);
  const unsigned lookupCount = lookups.u16(0);
  // Counts are 16-bit fields with no artificial low cap: `.has()` below
  // prevents out-of-bounds reads, while the selected-language walk and the
  // total `work` budget bound the expensive operations. Capping feature
  // indices here would reject ordinary large fonts whose active `liga`
  // feature happens to occur late in the FeatureList.
  if (!features.has(2, size_t(featureCount) * 6) || !lookups.has(2, size_t(lookupCount) * 2)) return 0;

  // A FeatureList is a pool, not an activation list. Only the selected
  // script's DefaultLangSys may activate a feature for this small Latin
  // shaping API. Prefer `latn`; DFLT is the fallback for fonts without a
  // usable Latin DefaultLangSys. Named language systems are intentionally not
  // selected because the API has no language parameter.
  auto usableLangSys = [](View langSys) {
    if (!langSys.has(0, 6)) return false;
    return langSys.has(6, size_t(langSys.u16(4)) * 2);
  };
  const unsigned scriptCount = scripts.u16(0);
  if (!scripts.has(2, size_t(scriptCount) * 6)) return 0;

  // ScriptRecords are required to be sorted by ScriptTag. Use bounded binary
  // searches rather than walking a hostile 65,535-record list on every query;
  // malformed/unsorted input may fail to select a script, but it must still
  // remain bounded and memory-safe.
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

  auto processFeature = [&](unsigned featureIndex) -> uint32_t {
    if (featureIndex >= featureCount || ++work > 4096) return 0;
    const size_t record = 2 + featureIndex * 6;
    if (memcmp(features.data + record, "liga", 4) != 0 && memcmp(features.data + record, "rlig", 4) != 0) return 0;
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
      if ((type != 4 && type != 7) || !lookup.has(6, size_t(subtableCount) * 2)) continue;
      for (unsigned j = 0; j < subtableCount; ++j) {
        if (++work > 4096) return 0;
        View subtable = lookup.sub(lookup.u16(6 + j * 2));
        if (type == 7) {
          // Extension Substitution: format 1 wraps another lookup type at a
          // 32-bit offset (used when the table is too large for 16-bit
          // offsets). Unwrap it if it wraps a ligature-substitution (4).
          if (!subtable.has(0, 8) || subtable.u16(0) != 1 || subtable.u16(2) != 4) continue;
          subtable = subtable.sub(subtable.u32(4));
        }
        if (subtable.u16(0) != 1) continue;  // LigatureSubstFormat1 only
        const int coverage = coverageIndex(subtable.sub(subtable.u16(2)), sequence[0]);
        const unsigned setCount = subtable.u16(4);
        if (coverage < 0 || unsigned(coverage) >= setCount || !subtable.has(6, size_t(setCount) * 2)) continue;
        View set = subtable.sub(subtable.u16(6 + unsigned(coverage) * 2));
        const unsigned ligatureCount = set.u16(0);
        if (!set.has(2, size_t(ligatureCount) * 2)) continue;
        for (unsigned k = 0; k < ligatureCount; ++k) {
          if (++work > 4096) return 0;
          View ligature = set.sub(set.u16(2 + k * 2));
          // LigatureGlyph, LigatureGlyphCount, then (LigatureGlyphCount-1)
          // ComponentGlyphIDs (the coverage glyph itself is implicit).
          if (ligature.u16(2) != length || !ligature.has(4, (length - 1) * 2)) continue;
          bool match = true;
          for (unsigned component = 1; component < length; ++component)
            match = match && ligature.u16(4 + (component - 1) * 2) == sequence[component];
          if (match) return ligature.u16(0);
        }
      }
    }
    return 0;
  };

  if (requiredFeatureIndex != 0xFFFFu) {
    const uint32_t result = processFeature(requiredFeatureIndex);
    if (result != 0) return result;
  }
  for (unsigned i = 0; i < featureIndexCount; ++i) {
    const uint32_t result = processFeature(langSys.u16(6 + i * 2));
    if (result != 0) return result;
  }
  return 0;
}

}  // namespace gsub
}  // namespace font
}  // namespace freeink
