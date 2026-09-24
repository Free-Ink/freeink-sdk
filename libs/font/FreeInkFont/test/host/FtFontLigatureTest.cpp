// Host smoke test for FtFont::ligature()/ligatureGlyphId(). Run via
// test/host/run_ligature.sh under ASan/UBSan against real bundled fonts.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "FtFont.h"
#include "Gsub.h"

using freeink::font::FtFont;

namespace {

std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (fread(data.data(), 1, data.size(), f) != data.size()) {
    fprintf(stderr, "short read on %s\n", path);
    exit(1);
  }
  fclose(f);
  return data;
}

int checks = 0;
int failures = 0;
void expect(bool cond, const char* what) {
  ++checks;
  if (!cond) {
    ++failures;
    fprintf(stderr, "FAIL: %s\n", what);
  } else {
    printf("ok: %s\n", what);
  }
}

uint32_t readU32be(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
void writeU16be(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
  bytes[offset] = uint8_t(value >> 8);
  bytes[offset + 1] = uint8_t(value);
}
void writeU32be(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Builds a byte-identical copy of `bytes` except its sfnt table directory's
// 'GSUB' entry has its length zeroed, so FT_Load_Sfnt_Table reports length 0
// for it (this library's own "no GSUB" signal) while every other table
// (cmap, glyf, ...) stays fully valid and parseable. This gives the
// deinit()/re-init regression test below a genuinely different "font" — one
// whose glyph IDs are identical but whose ligature answers must all become 0
// — without needing a second binary font fixture: the only other font in
// this repo is the same DejaVuSans.ttf, so re-init with unmodified bytes
// can't tell a correctly-reloaded cache apart from a stale one that happens
// to still hold the right answer.
std::vector<uint8_t> stripGsubTable(std::vector<uint8_t> bytes) {
  if (bytes.size() < 12) return bytes;
  const unsigned numTables = (unsigned(bytes[4]) << 8) | bytes[5];
  for (unsigned i = 0; i < numTables; ++i) {
    const size_t record = 12 + size_t(i) * 16;
    if (record + 16 > bytes.size()) break;
    if (memcmp(&bytes[record], "GSUB", 4) == 0) {
      writeU32be(&bytes[record + 12], 0);  // length field
      return bytes;
    }
  }
  fprintf(stderr, "test setup error: no GSUB table found to strip\n");
  exit(1);
}

void appendU16be(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(uint8_t(value >> 8));
  bytes.push_back(uint8_t(value));
}
void appendTag(std::vector<uint8_t>& bytes, const char* tag) { bytes.insert(bytes.end(), tag, tag + 4); }

// Build the smallest valid GSUB table containing one glyph-1 + glyph-2 ->
// glyph-42 `liga` rule. The script/language switches let the parser tests
// distinguish activation from merely finding a pooled FeatureList record.
std::vector<uint8_t> syntheticGsub(const char* scriptTag, bool defaultPresent, bool defaultActive, bool namedActive) {
  std::vector<uint8_t> bytes(10, 0);

  const size_t scriptList = bytes.size();
  appendU16be(bytes, 1);  // ScriptCount
  appendTag(bytes, scriptTag);
  appendU16be(bytes, 0);  // ScriptRecord offset placeholder
  const size_t script = bytes.size();
  appendU16be(bytes, 0);  // DefaultLangSys offset placeholder
  appendU16be(bytes, namedActive ? 1 : 0);
  const size_t namedRecord = bytes.size();
  if (namedActive) {
    appendTag(bytes, "ENG ");
    appendU16be(bytes, 0);  // LangSysRecord offset placeholder
  }
  const size_t defaultLangSys = bytes.size();
  if (defaultPresent) {
    appendU16be(bytes, 0);       // LookupOrder
    appendU16be(bytes, 0xFFFF);  // RequiredFeatureIndex
    appendU16be(bytes, defaultActive ? 1 : 0);
    if (defaultActive) appendU16be(bytes, 0);
  }
  const size_t namedLangSys = bytes.size();
  if (namedActive) {
    appendU16be(bytes, 0);
    appendU16be(bytes, 0xFFFF);
    appendU16be(bytes, 1);
    appendU16be(bytes, 0);
  }
  writeU16be(bytes, scriptList + 6, uint16_t(script - scriptList));
  if (defaultPresent) writeU16be(bytes, script, uint16_t(defaultLangSys - script));
  if (namedActive) writeU16be(bytes, namedRecord + 4, uint16_t(namedLangSys - script));

  const size_t featureList = bytes.size();
  appendU16be(bytes, 1);  // FeatureCount
  appendTag(bytes, "liga");
  appendU16be(bytes, 8);  // FeatureRecord offset
  appendU16be(bytes, 0);  // FeatureParams offset
  appendU16be(bytes, 1);  // LookupListIndexCount
  appendU16be(bytes, 0);  // lookup 0

  const size_t lookupList = bytes.size();
  appendU16be(bytes, 1);   // LookupCount
  appendU16be(bytes, 4);   // LookupRecord offset
  appendU16be(bytes, 4);   // LookupType: LigatureSubst
  appendU16be(bytes, 0);   // LookupFlag
  appendU16be(bytes, 1);   // SubTableCount
  appendU16be(bytes, 8);   // subtable offset
  appendU16be(bytes, 1);   // LigatureSubstFormat1
  appendU16be(bytes, 8);   // Coverage offset
  appendU16be(bytes, 1);   // LigatureSetCount
  appendU16be(bytes, 14);  // LigatureSet offset (after CoverageTable)
  appendU16be(bytes, 1);   // CoverageFormat1
  appendU16be(bytes, 1);   // GlyphCount
  appendU16be(bytes, 1);   // covered glyph
  appendU16be(bytes, 1);   // LigatureCount
  appendU16be(bytes, 4);   // Ligature offset
  appendU16be(bytes, 42);  // LigatureGlyph
  appendU16be(bytes, 2);   // ComponentCount
  appendU16be(bytes, 2);   // second component glyph

  writeU16be(bytes, 4, uint16_t(scriptList));
  writeU16be(bytes, 6, uint16_t(featureList));
  writeU16be(bytes, 8, uint16_t(lookupList));
  return bytes;
}

// Two active features share the same 1+2 input but substitute distinct glyphs;
// a third, unsupported feature can occupy requiredFeatureIndex to exercise the
// "required first, then listed order" rules without depending on a real font.
std::vector<uint8_t> syntheticFeatureOrderGsub(uint16_t requiredFeatureIndex, const uint16_t* listedIndices,
                                               unsigned listedCount) {
  std::vector<uint8_t> bytes(10, 0);

  const size_t scriptList = bytes.size();
  appendU16be(bytes, 1);
  appendTag(bytes, "latn");
  appendU16be(bytes, 0);
  const size_t script = bytes.size();
  const size_t defaultOffset = bytes.size();
  appendU16be(bytes, 0);
  appendU16be(bytes, 0);  // LangSysCount: no named language systems
  const size_t defaultLangSys = bytes.size();
  appendU16be(bytes, 0);  // LookupOrder
  appendU16be(bytes, requiredFeatureIndex);
  appendU16be(bytes, static_cast<uint16_t>(listedCount));
  for (unsigned i = 0; i < listedCount; ++i) appendU16be(bytes, listedIndices[i]);
  writeU16be(bytes, scriptList + 6, uint16_t(script - scriptList));
  writeU16be(bytes, defaultOffset, uint16_t(defaultLangSys - script));

  const size_t featureList = bytes.size();
  appendU16be(bytes, 3);
  const char* tags[] = {"liga", "rlig", "kern"};
  size_t featureRecords[3];
  for (unsigned i = 0; i < 3; ++i) {
    appendTag(bytes, tags[i]);
    featureRecords[i] = bytes.size();
    appendU16be(bytes, 0);
  }
  for (unsigned i = 0; i < 3; ++i) {
    const size_t feature = bytes.size();
    appendU16be(bytes, 0);  // FeatureParams
    appendU16be(bytes, i < 2 ? 1 : 0);
    if (i < 2) appendU16be(bytes, static_cast<uint16_t>(i));
    writeU16be(bytes, featureRecords[i], uint16_t(feature - featureList));
  }

  const size_t lookupList = bytes.size();
  appendU16be(bytes, 2);
  const size_t lookupRecords[2] = {bytes.size(), bytes.size() + 2};
  appendU16be(bytes, 0);
  appendU16be(bytes, 0);
  for (unsigned i = 0; i < 2; ++i) {
    const size_t lookup = bytes.size();
    writeU16be(bytes, lookupRecords[i], uint16_t(lookup - lookupList));
    appendU16be(bytes, 4);   // LookupType: LigatureSubst
    appendU16be(bytes, 0);   // LookupFlag
    appendU16be(bytes, 1);   // SubTableCount
    appendU16be(bytes, 8);   // subtable offset
    appendU16be(bytes, 1);   // LigatureSubstFormat1
    appendU16be(bytes, 8);   // Coverage offset
    appendU16be(bytes, 1);   // LigatureSetCount
    appendU16be(bytes, 14);  // LigatureSet offset
    appendU16be(bytes, 1);   // CoverageFormat1
    appendU16be(bytes, 1);   // GlyphCount
    appendU16be(bytes, 1);   // covered glyph
    appendU16be(bytes, 1);   // LigatureCount
    appendU16be(bytes, 4);   // Ligature offset
    appendU16be(bytes, static_cast<uint16_t>(42 + i));
    appendU16be(bytes, 2);  // ComponentCount
    appendU16be(bytes, 2);  // second component glyph
  }

  writeU16be(bytes, 4, uint16_t(scriptList));
  writeU16be(bytes, 6, uint16_t(featureList));
  writeU16be(bytes, 8, uint16_t(lookupList));
  return bytes;
}

// Keep the top-level FeatureList/LookupList valid while making ScriptList
// deliberately huge and unsorted. The parser must reject this malformed
// 65,535-entry list without a linear unbounded walk.
std::vector<uint8_t> pathologicalScriptListGsub() {
  std::vector<uint8_t> bytes(10, 0);
  const size_t featureList = bytes.size();
  appendU16be(bytes, 0);
  const size_t lookupList = bytes.size();
  appendU16be(bytes, 0);
  const size_t scriptList = bytes.size();
  appendU16be(bytes, 0xFFFF);
  const size_t script = bytes.size();
  appendU16be(bytes, 0);  // no DefaultLangSys
  appendU16be(bytes, 0);  // no named LangSys records
  for (unsigned i = 0; i < 0xFFFF; ++i) {
    appendTag(bytes, i == 0 ? "ZZZZ" : "AAAA");
    appendU16be(bytes, uint16_t(script - scriptList));
  }
  writeU16be(bytes, 4, uint16_t(scriptList));
  writeU16be(bytes, 6, uint16_t(featureList));
  writeU16be(bytes, 8, uint16_t(lookupList));
  return bytes;
}

std::vector<uint8_t> oversizedBorrowedGsubFont(std::vector<uint8_t> bytes) {
  if (bytes.size() < 12) return bytes;
  const unsigned numTables = (unsigned(bytes[4]) << 8) | bytes[5];
  constexpr uint32_t oversizedLength = 1024 * 1024 + 1;
  for (unsigned i = 0; i < numTables; ++i) {
    const size_t record = 12 + size_t(i) * 16;
    if (record + 16 > bytes.size()) break;
    if (memcmp(&bytes[record], "GSUB", 4) != 0) continue;
    const size_t tableOffset = readU32be(&bytes[record + 8]);
    const size_t requiredSize = tableOffset + oversizedLength;
    if (bytes.size() < requiredSize) bytes.resize(requiredSize, 0);
    writeU32be(&bytes[record + 12], oversizedLength);
    return bytes;
  }
  fprintf(stderr, "test setup error: no GSUB table found to enlarge\n");
  exit(1);
}

struct MemoryReader {
  const uint8_t* data;
  size_t size;
};
unsigned long readMemory(void* context, unsigned long offset, unsigned char* buffer, unsigned long count) {
  const auto* reader = static_cast<const MemoryReader*>(context);
  if (offset >= reader->size) return 0;
  const size_t available = reader->size - offset;
  const size_t copied = count < available ? count : available;
  memcpy(buffer, reader->data + offset, copied);
  return static_cast<unsigned long>(copied);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s font.ttf\n", argv[0]);
    return 1;
  }
  const std::vector<uint8_t> bytes = readFile(argv[1]);

  FtFont font;
  expect(font.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16, 400, false), "init() loads the face");

  // Raw glyph-ID path: 'f'+'i' should resolve to *some* nonzero glyph via the
  // font's real GSUB table (not a hardcoded guess) whenever the font ships a
  // 'liga'/'rlig' feature that covers it — this is the unrestricted form a
  // glyph-indexed consumer (e.g. CrossInk's own page cache) can rely on even
  // when the substituted glyph has no cmap entry.
  const uint32_t fi[2] = {'f', 'i'};
  const uint32_t glyphFi = font.ligatureGlyphId(fi, 2);
  expect(glyphFi != 0, "ligatureGlyphId('f','i') finds a real GSUB substitution in DejaVuSans");

  // Nonsense pairs must never fabricate a ligature.
  const uint32_t nonsense[2] = {'q', 'z'};
  expect(font.ligatureGlyphId(nonsense, 2) == 0, "ligatureGlyphId('q','z') finds nothing");

  // DejaVuSans.ttf is a committed, deterministic fixture — these are its real
  // values, not a guess. A regression to "always return 0" (or to the
  // pre-fix "always return 0xFB00 unless the pair is exactly fi/fl") must
  // fail these, unlike the disjunctions this replaced.
  expect(font.ligature('f', 'i', 0) == 0xFB01, "ligature('f','i') is exactly 0xFB01 in DejaVuSans");
  expect(font.ligature('f', 'f', 0) == 0xFB00, "ligature('f','f') is exactly 0xFB00 in DejaVuSans");
  expect(font.ligature('q', 'z', 0) == 0, "ligature('q','z') is not a ligature");
  expect(font.ligature(0xFB00, 'x', 0) == 0,
         "chained ligature(0xFB00,'x') is 0 — not every right side after 'ff' is a ligature");

  // The 3-glyph chained form ("ff"+'i' -> "ffi") must route back to the
  // original 3 letters for GSUB, not query GSUB for a rule on the "ff"
  // glyph itself (see the comment in FtFont::ligature()).
  const uint32_t ffi[3] = {'f', 'f', 'i'};
  expect(font.ligatureGlyphId(ffi, 3) != 0, "ligatureGlyphId('f','f','i') finds DejaVuSans's real 3-glyph rule");
  expect(font.ligature(0xFB00, 'i', 0) == 0xFB03, "chained ligature(0xFB00,'i') is exactly 0xFB03 in DejaVuSans");
  expect(font.ligature(0xFB00, 'l', 0) == 0xFB04, "chained ligature(0xFB00,'l') is exactly 0xFB04 in DejaVuSans");

  // deinit()/re-init must not leave a stale GSUB table behind: glyph IDs are
  // face-local, so answering a NEW face's ligature query with the OLD face's
  // cached table would resolve against the wrong font.
  font.deinit();
  expect(font.ligatureGlyphId(fi, 2) == 0, "a deinit() face reports no ligatures rather than a stale answer");
  expect(font.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16, 400, false),
         "re-init() after deinit() succeeds");
  expect(font.ligature('f', 'i', 0) == 0xFB01, "ligature() is correct again after deinit()/re-init() with same bytes");

  // Same content re-init can't distinguish "correctly reloaded" from "stale
  // pointer that happens to still be right" — a genuinely different font is
  // needed. Built from a copy of the same bytes with its GSUB table's length
  // zeroed (see stripGsubTable()): identical glyph IDs, but zero ligatures.
  // Calling init() again WITHOUT an intervening deinit() (the exact path the
  // bug lived in — only deinit() reset the cache before this fix) must not
  // answer with the previous, real-GSUB face's cached results.
  const std::vector<uint8_t> strippedBytes = stripGsubTable(bytes);
  expect(font.ligature('f', 'i', 0) == 0xFB01, "font still has the real GSUB answer right before re-init");
  expect(font.init(strippedBytes.data(), static_cast<uint32_t>(strippedBytes.size()), 16, 400, false),
         "init() with different (GSUB-stripped) bytes succeeds, without a deinit() in between");
  expect(font.ligatureGlyphId(fi, 2) == 0,
         "ligatureGlyphId('f','i') is 0 on the GSUB-stripped face — not the previous face's stale glyph ID");
  expect(font.ligature('f', 'i', 0) == 0,
         "ligature('f','i') is 0 on the GSUB-stripped face — not the previous face's stale 0xFB01");

  // And restoring the original bytes (again without deinit() in between)
  // must bring the real answer back, proving the reload actually happens
  // both directions rather than the cache just staying permanently cleared.
  expect(font.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16, 400, false),
         "init() back to the original bytes succeeds, again without a deinit() in between");
  expect(font.ligature('f', 'i', 0) == 0xFB01, "ligature('f','i') is correct again after switching fonts twice");

  // Gsub::LigatureGlyphId is a public header advertising bounds-checked
  // behavior against "malformed/hostile font data" — a null table pointer
  // with a nonzero claimed size (e.g. from a failed read) must degrade to 0,
  // not dereference the null pointer.
  const uint32_t pair[2] = {'f', 'i'};
  expect(freeink::font::gsub::LigatureGlyphId(nullptr, 100, pair, 2) == 0,
         "Gsub::LigatureGlyphId(nullptr, nonzero size, ...) returns 0 instead of crashing");

  const uint32_t syntheticPair[2] = {1, 2};
  const std::vector<uint8_t> unrelatedLanguage = syntheticGsub("latn", true, false, true);
  expect(
      freeink::font::gsub::LigatureGlyphId(unrelatedLanguage.data(), unrelatedLanguage.size(), syntheticPair, 2) == 0,
      "a liga feature active only in a named language system is ignored");
  const std::vector<uint8_t> unrelatedScript = syntheticGsub("arab", true, true, false);
  expect(freeink::font::gsub::LigatureGlyphId(unrelatedScript.data(), unrelatedScript.size(), syntheticPair, 2) == 0,
         "a liga feature active only in an unrelated script is ignored");
  const std::vector<uint8_t> activeLatin = syntheticGsub("latn", true, true, false);
  expect(freeink::font::gsub::LigatureGlyphId(activeLatin.data(), activeLatin.size(), syntheticPair, 2) == 42,
         "latn DefaultLangSys activates the liga feature");
  const std::vector<uint8_t> activeDefault = syntheticGsub("DFLT", true, true, false);
  expect(freeink::font::gsub::LigatureGlyphId(activeDefault.data(), activeDefault.size(), syntheticPair, 2) == 42,
         "DFLT DefaultLangSys activates the liga feature as fallback");

  std::vector<uint8_t> malformed = activeLatin;
  writeU16be(malformed, 4, 0xFFFF);  // ScriptList offset outside the table
  expect(freeink::font::gsub::LigatureGlyphId(malformed.data(), malformed.size(), syntheticPair, 2) == 0,
         "an out-of-range ScriptList offset returns no ligature");
  std::vector<uint8_t> truncated(activeLatin.begin(), activeLatin.begin() + 10);
  expect(freeink::font::gsub::LigatureGlyphId(truncated.data(), truncated.size(), syntheticPair, 2) == 0,
         "a truncated GSUB header returns no ligature");

  const uint16_t requiredListed[] = {0};
  const std::vector<uint8_t> requiredWins = syntheticFeatureOrderGsub(1, requiredListed, 1);
  expect(freeink::font::gsub::LigatureGlyphId(requiredWins.data(), requiredWins.size(), syntheticPair, 2) == 43,
         "requiredFeatureIndex is evaluated before listed features when it matches");
  const uint16_t listedReverse[] = {1, 0};
  const std::vector<uint8_t> listedOrder = syntheticFeatureOrderGsub(2, listedReverse, 2);
  expect(freeink::font::gsub::LigatureGlyphId(listedOrder.data(), listedOrder.size(), syntheticPair, 2) == 43,
         "listed features are evaluated in their declared order after a non-matching required feature");
  const uint16_t listedForward[] = {0, 1};
  const std::vector<uint8_t> listedForwardOrder = syntheticFeatureOrderGsub(2, listedForward, 2);
  expect(freeink::font::gsub::LigatureGlyphId(listedForwardOrder.data(), listedForwardOrder.size(), syntheticPair, 2) ==
             42,
         "reversing listed feature order selects the first matching substitution");

  const std::vector<uint8_t> pathologicalScripts = pathologicalScriptListGsub();
  expect(freeink::font::gsub::LigatureGlyphId(pathologicalScripts.data(), pathologicalScripts.size(), syntheticPair,
                                              2) == 0,
         "a 65,535-record ScriptList rejects safely without an unbounded scan");

  const std::vector<uint8_t> oversizedBorrowed = oversizedBorrowedGsubFont(bytes);
  FtFont oversizedFace;
  expect(oversizedFace.init(oversizedBorrowed.data(), static_cast<uint32_t>(oversizedBorrowed.size()), 16, 400, false),
         "a padded oversized borrowed GSUB font still initializes");
  expect(oversizedFace.ligatureGlyphId(fi, 2) == 0,
         "an oversized in-bounds borrowed GSUB table is rejected by the parse ceiling");

  MemoryReader reader{bytes.data(), bytes.size()};
  FtFont budgetLimitedStream;
  budgetLimitedStream.setGsubByteBudget(0);
  expect(budgetLimitedStream.initStream(&readMemory, &reader, static_cast<unsigned long>(bytes.size()), 16, 400, false),
         "initStream() loads the face with a zero GSUB budget");
  expect(budgetLimitedStream.ligatureGlyphId(fi, 2) == 0,
         "a streamed GSUB table rejected by the caller budget degrades to no "
         "ligature");

  FtFont releasedStream;
  expect(releasedStream.initStream(&readMemory, &reader, static_cast<unsigned long>(bytes.size()), 16, 400, false),
         "initStream() loads the face with the default GSUB budget");
  expect(releasedStream.ligatureGlyphId(fi, 2) != 0, "a streamed face resolves GSUB before release");
  releasedStream.releaseLigatureTable();
  expect(releasedStream.ligatureGlyphId(fi, 2) == 0, "releaseLigatureTable() drops the owned streamed table");

  printf("\n%d/%d checks passed\n", checks - failures, checks);
  return failures == 0 ? 0 : 1;
}
