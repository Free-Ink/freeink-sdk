#include "FtFont.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <climits>
#include <cstring>

#include "FontAlloc.h"
#include "Gpos.h"
#include "Gsub.h"

namespace freeink {
namespace font {

namespace {
FtFont::MemoryCallbacks g_memoryCallbacks{};

void* fontAlloc(const size_t size) {
  return g_memoryCallbacks.allocate ? g_memoryCallbacks.allocate(g_memoryCallbacks.context, size) : fiFontMalloc(size);
}
void fontFree(void* block) {
  if (g_memoryCallbacks.deallocate)
    g_memoryCallbacks.deallocate(g_memoryCallbacks.context, block);
  else
    fiFontFree(block);
}
void* fontRealloc(void* block, const size_t oldSize, const size_t newSize) {
  return g_memoryCallbacks.reallocate ? g_memoryCallbacks.reallocate(g_memoryCallbacks.context, block, oldSize, newSize)
                                      : fiFontRealloc(block, newSize);
}

// FreeType memory hooks use either the caller's bounded allocator or the
// platform's PSRAM-preferring default. The shared library is initialized only
// once, so configureMemory() deliberately freezes this choice before first use.
void* ftAlloc(FT_Memory, long size) { return size > 0 ? fontAlloc(static_cast<size_t>(size)) : nullptr; }
void ftFree(FT_Memory, void* block) { fontFree(block); }
void* ftRealloc(FT_Memory, long currentSize, long newSize, void* block) {
  if (currentSize < 0 || newSize < 0) return nullptr;
  return fontRealloc(block, static_cast<size_t>(currentSize), static_cast<size_t>(newSize));
}

// One shared FreeType library for all faces; the library object itself is tiny.
FT_Library g_lib = nullptr;
FT_MemoryRec_ g_ftMemory{};
bool ensureLib() {
  if (g_lib) return true;
  g_ftMemory.user = nullptr;
  g_ftMemory.alloc = &ftAlloc;
  g_ftMemory.free = &ftFree;
  g_ftMemory.realloc = &ftRealloc;
  if (FT_New_Library(&g_ftMemory, &g_lib) != 0) return false;
  FT_Add_Default_Modules(g_lib);  // register the sfnt/truetype/smooth/... modules
  return true;
}
constexpr uint32_t kTagWght = FT_MAKE_TAG('w', 'g', 'h', 't');
constexpr uint32_t kTagItal = FT_MAKE_TAG('i', 't', 'a', 'l');
constexpr uint32_t kTagSlnt = FT_MAKE_TAG('s', 'l', 'n', 't');
constexpr uint32_t kTagGsub = FT_MAKE_TAG('G', 'S', 'U', 'B');
constexpr uint32_t kTagGpos = FT_MAKE_TAG('G', 'P', 'O', 'S');
constexpr uint32_t kTagTtcf = FT_MAKE_TAG('t', 't', 'c', 'f');

uint16_t readBe16(const uint8_t* data) { return uint16_t(data[0]) * 256u + data[1]; }
uint32_t readBe32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}

bool findSfntTable(const uint8_t* data, size_t size, uint32_t tag, const uint8_t** table, size_t* tableSize) {
  if (data == nullptr || table == nullptr || tableSize == nullptr || size < 12) return false;
  size_t directory = 0;
  if (readBe32(data) == kTagTtcf) {
    if (size < 16 || readBe32(data + 8) == 0 || readBe32(data + 8) > (size - 12) / 4) return false;
    directory = readBe32(data + 12);
  }
  if (directory > size || size - directory < 12) return false;
  const size_t records = directory + 12;
  const unsigned count = readBe16(data + directory + 4);
  if (count > (size - records) / 16) return false;
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t* record = data + records + size_t(i) * 16;
    if (readBe32(record) != tag) continue;
    const size_t offset = readBe32(record + 8);
    const size_t length = readBe32(record + 12);
    if (length == 0 || offset > size || length > size - offset) return false;
    *table = data + offset;
    *tableSize = length;
    return true;
  }
  return false;
}

// #if, not #ifdef: ftmodule.h/ftoption.h gate the actual modules on the
// VALUE of these macros (so `-DFREEINK_FONT_ENABLE_MONOCHROME=0`, a common
// PlatformIO idiom for "explicitly off", compiles nothing in), and these
// capability flags have to agree exactly or setRenderOptions() reports a
// mode as supported when the module that would actually serve it isn't
// there — confirmed: with #ifdef, that exact build config still returned
// true for monochrome while every glyph rasterized to nullptr.
#if FREEINK_FONT_ENABLE_AUTOHINT
constexpr bool kAutohintCompiled = true;
#else
constexpr bool kAutohintCompiled = false;
#endif
#if FREEINK_FONT_ENABLE_NATIVE_HINTING
constexpr bool kNativeHintingCompiled = true;
#else
constexpr bool kNativeHintingCompiled = false;
#endif
#if FREEINK_FONT_ENABLE_MONOCHROME
constexpr bool kMonochromeCompiled = true;
#else
constexpr bool kMonochromeCompiled = false;
#endif

// Two independent axes: the antialiasing TARGET (Normal/Light/Mono — these
// three occupy the same FT_LOAD_TARGET_ bitfield and are mutually exclusive
// with EACH OTHER, so only one is picked), and hint SUPPRESSION/forcing
// (NO_HINTING, NO_AUTOHINT, FORCE_AUTOHINT — independent bits, composed on
// top).
//
// FT_LOAD_NO_HINTING disables ALL hinting, including a harmless, unconditional
// part of it that has nothing to do with any optional module: FreeType's
// TrueType loader always pixel-ROUNDS phantom-point advances when hinting is
// on (ttgload.c's IS_HINTED, gated purely by this one bit), even with the
// bytecode interpreter uncompiled — that's the sub-pixel-accurate advance
// rounding every existing build has always shipped with. Adding NO_HINTING to
// "Default" (an earlier version of this fix did exactly that) silently
// truncates every advance instead of rounding it, changing pagination for
// every caller, in every build — worse than the bug it was meant to close.
//
// FT_LOAD_NO_AUTOHINT is the actually-correct "opt-in has no side effect"
// bit: it blocks FreeType's documented fallback (use the auto-hinter when the
// driver has no native hinter of its own) without touching phantom-point
// rounding. Default sets ONLY this — confirmed byte-identical (0/1615 advance
// and bitmap diffs on a real font) to this library's original, pre-RenderOptions
// FT_LOAD_DEFAULT behavior, in both a build with the autofit/native modules
// compiled and one without. None is a distinct, EXPLICIT opt-in for "truly no
// hinting, not even phantom-point rounding" — safe to be more aggressive than
// Default because a caller has to ask for it by name.
FT_Int32 loadFlagsFor(const FtFont::RenderOptions& options) {
  FT_Int32 flags = FT_LOAD_NO_BITMAP;
  if (options.monochrome) {
    flags |= FT_LOAD_TARGET_MONO;
  } else if (options.hinting == FtFont::HintingMode::Light) {
    flags |= FT_LOAD_TARGET_LIGHT;  // FreeType always auto-hints under LIGHT
  } else {
    flags |= FT_LOAD_TARGET_NORMAL;
  }
  switch (options.hinting) {
    case FtFont::HintingMode::Auto:
      flags |= FT_LOAD_FORCE_AUTOHINT;
      break;
    case FtFont::HintingMode::None:
      flags |= FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
      break;
    case FtFont::HintingMode::Light:
      // TARGET_LIGHT forces auto-hint on its own — except monochrome output
      // above already claimed the target slot LIGHT would have used (in
      // which case this reduces to Auto+mono; there is no separate "light"
      // render mode to preserve), so ask for it explicitly under mono too.
      if (options.monochrome) flags |= FT_LOAD_FORCE_AUTOHINT;
      break;
    case FtFont::HintingMode::Native:
    case FtFont::HintingMode::Default:
    default:
      // Identical flags by necessity, not by accident: once a native hinter
      // is compiled in and registered, FreeType uses it automatically for
      // any hinted (non-NO_HINTING) load unless autohint is force-requested
      // — there is no third way to say "use native but only if I explicitly
      // asked", so Native and Default only diverge through which interpreter
      // version gets applied (see applyGlobalProperties()), not through
      // these flags. Both explicitly block the autohint fallback so that
      // compiling FREEINK_FONT_ENABLE_AUTOHINT in never changes a Default
      // caller's output.
      flags |= FT_LOAD_NO_AUTOHINT;
      break;
  }
  return flags;
}

FT_Render_Mode renderModeFor(const FtFont::RenderOptions& options) {
  return options.monochrome ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
}

struct StreamCtx {
  FtFont::ReadFn read;
  void* ctx;
  bool failed;
};

// FT_Stream io hook: FreeType asks for `count` bytes at absolute `offset`.
// count == 0 is a seek FreeType tracks itself, so nothing to do.
unsigned long ftStreamIo(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) {
  if (count == 0) return 0;
  auto* c = static_cast<StreamCtx*>(stream->descriptor.pointer);
  const unsigned long actual = c->read(c->ctx, offset, buffer, count);
  if (actual != count) c->failed = true;
  return actual;
}
// The source (e.g. an SD file) is owned by the caller, not FreeType.
void ftStreamClose(FT_Stream) {}

bool supportedFace(FT_Face face) {
  return face && FT_IS_SCALABLE(face) && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0;
}

void describeFace(FT_Face face, FtFont::FaceInfo& info, char* family, const size_t familyCapacity) {
  info = FtFont::FaceInfo{};
  const char* name = face->family_name ? face->family_name : "";
  info.familyLength = std::strlen(name);
  if (familyCapacity) {
    const size_t copied = std::min(info.familyLength, familyCapacity - 1);
    if (copied) std::memcpy(family, name, copied);
    family[copied] = '\0';
  }
  const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
  info.weight = os2 ? os2->usWeightClass : ((face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400);
  info.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
}

int32_t fixed16_16To26_6(const long value) {
  const int64_t wide = value;
  const int64_t rounded = wide >= 0 ? (wide + 512) >> 10 : -((-wide + 512) >> 10);
  return int32_t(std::clamp<int64_t>(rounded, INT32_MIN, INT32_MAX));
}
}  // namespace

bool FtFont::configureMemory(const MemoryCallbacks* callbacks) {
  if (g_lib) return false;
  if (!callbacks) {
    g_memoryCallbacks = MemoryCallbacks{};
    return true;
  }
  if (!callbacks->allocate || !callbacks->deallocate || !callbacks->reallocate) return false;
  g_memoryCallbacks = *callbacks;
  return true;
}

FtFont::InspectResult FtFont::inspectMemory(const uint8_t* data, const uint32_t length, FaceInfo& info, char* family,
                                            const size_t familyCapacity) {
  info = FaceInfo{};
  if (familyCapacity && !family) return InspectResult::Unsupported;
  if (familyCapacity) family[0] = '\0';
  if (!data || !length) return InspectResult::Unsupported;
  if (!ensureLib()) return InspectResult::Unavailable;

  FT_Face face = nullptr;
  const FT_Error error = FT_New_Memory_Face(g_lib, data, static_cast<FT_Long>(length), 0, &face);
  if (error) return error == FT_Err_Out_Of_Memory ? InspectResult::Unavailable : InspectResult::Unsupported;
  const bool valid = supportedFace(face);
  if (valid) describeFace(face, info, family, familyCapacity);
  FT_Done_Face(face);
  return valid ? InspectResult::Ok : InspectResult::Unsupported;
}

FtFont::InspectResult FtFont::inspectStream(const ReadFn read, void* ctx, const unsigned long fileSize, FaceInfo& info,
                                            char* family, const size_t familyCapacity) {
  info = FaceInfo{};
  if (familyCapacity && !family) return InspectResult::Unsupported;
  if (familyCapacity) family[0] = '\0';
  if (!read || !fileSize) return InspectResult::Unsupported;
  if (!ensureLib()) return InspectResult::Unavailable;

  StreamCtx source{read, ctx, false};
  FT_StreamRec stream{};
  stream.size = fileSize;
  stream.descriptor.pointer = &source;
  stream.read = &ftStreamIo;
  stream.close = &ftStreamClose;
  FT_Open_Args args{};
  args.flags = FT_OPEN_STREAM;
  args.stream = &stream;
  FT_Face face = nullptr;
  const FT_Error error = FT_Open_Face(g_lib, &args, 0, &face);
  if (error) {
    return source.failed || error == FT_Err_Out_Of_Memory ? InspectResult::Unavailable : InspectResult::Unsupported;
  }
  const bool valid = supportedFace(face);
  if (valid) describeFace(face, info, family, familyCapacity);
  FT_Done_Face(face);
  return source.failed ? InspectResult::Unavailable : valid ? InspectResult::Ok : InspectResult::Unsupported;
}

FtFont::~FtFont() { deinit(); }

void FtFont::deinit() {
  if (face_) {
    FT_Done_Face(static_cast<FT_Face>(face_));
    face_ = nullptr;
  }
  fontFree(stream_);
  stream_ = nullptr;
  fontFree(streamCtx_);
  streamCtx_ = nullptr;
  ready_ = false;
  size26_6_ = 0;
  obliqueShear_ = false;
  options_ = RenderOptions{};
  freeMonoBuffer();
  // Glyph IDs are face-local: a stale GSUB table cached from the PREVIOUS
  // face would resolve the new face's glyph IDs against the wrong font,
  // silently either finding nothing or (worse) matching a coincidentally
  // reused ID for a wrong-but-real ligature. Must be cleared on every
  // deinit(), since init()/initStream() can rebuild this same instance with
  // completely different bytes.
  freeGsubTable();
  gsubLoadAttempted_ = false;
  freeGposTable();
  gposLoadAttempted_ = false;
  fontData_ = nullptr;
  fontDataSize_ = 0;
}

void FtFont::freeGsubTable() {
  if (gsubTableOwned_) fontFree(const_cast<uint8_t*>(gsubTable_));
  gsubTable_ = nullptr;
  gsubTableSize_ = 0;
  gsubTableOwned_ = false;
}

void FtFont::setGsubByteBudget(const size_t maxBytes) {
  gsubByteBudget_ = maxBytes;
  if (gsubTableOwned_ && gsubTableSize_ > gsubByteBudget_) releaseLigatureTable();
}

void FtFont::releaseLigatureTable() {
  freeGsubTable();
  gsubLoadAttempted_ = true;
}

void FtFont::freeGposTable() {
  if (gposTableOwned_) fontFree(const_cast<uint8_t*>(gposTable_));
  gposTable_ = nullptr;
  gposTableSize_ = 0;
  gposTableOwned_ = false;
}

void FtFont::setGposByteBudget(const size_t maxBytes) {
  gposByteBudget_ = maxBytes;
  if (gposTableOwned_ && gposTableSize_ > gposByteBudget_) releaseKerningTable();
}

void FtFont::releaseKerningTable() {
  freeGposTable();
  gposLoadAttempted_ = true;
}

bool FtFont::init(const uint8_t* data, const uint32_t len, const uint16_t sizePx, const int weight, const bool italic) {
  // Rebuild from a clean slate every time: this same instance can be
  // init()'d again with completely different bytes without an intervening
  // deinit() call, and glyph IDs (so the cached GSUB table) are face-local —
  // a stale table from the PREVIOUS face would resolve the new face's glyph
  // IDs against the wrong font. This also closes a pre-existing leak: a
  // second init() without deinit() previously dropped the old FT_Face
  // without ever calling FT_Done_Face on it.
  deinit();
  if (!ensureLib() || data == nullptr || len == 0) return false;
  fontData_ = data;
  fontDataSize_ = len;
  FT_Face face = nullptr;
  if (FT_New_Memory_Face(g_lib, data, static_cast<FT_Long>(len), 0, &face) != 0) {
    fontData_ = nullptr;
    fontDataSize_ = 0;
    return false;
  }
  face_ = face;
  return finishInit(sizePx, weight, italic);
}

bool FtFont::initStream(const ReadFn read, void* ctx, const unsigned long fileSize, const uint16_t sizePx,
                        const int weight, const bool italic) {
  deinit();  // see the comment in init() — same reasoning applies here
  if (!ensureLib() || read == nullptr || fileSize == 0) return false;

  // These wrappers must outlive the face, so they use the configured font
  // allocator rather than a small task stack frame. Both allocations are
  // bounded and failure is reported to the caller.
  auto* sc = static_cast<StreamCtx*>(fontAlloc(sizeof(StreamCtx)));
  auto* stream = static_cast<FT_StreamRec*>(fontAlloc(sizeof(FT_StreamRec)));
  if (!sc || !stream) {
    fontFree(stream);
    fontFree(sc);
    return false;
  }
  *sc = StreamCtx{read, ctx, false};
  std::memset(stream, 0, sizeof(*stream));
  stream->size = fileSize;
  stream->pos = 0;
  stream->descriptor.pointer = sc;
  stream->read = &ftStreamIo;
  stream->close = &ftStreamClose;
  streamCtx_ = sc;
  stream_ = stream;

  FT_Open_Args args{};
  args.flags = FT_OPEN_STREAM;
  args.stream = stream;
  FT_Face face = nullptr;
  if (FT_Open_Face(g_lib, &args, 0, &face) != 0) {
    fontFree(stream);
    fontFree(sc);
    stream_ = nullptr;
    streamCtx_ = nullptr;
    return false;
  }
  face_ = face;
  return finishInit(sizePx, weight, italic);
}

bool FtFont::finishInit(const uint16_t sizePx, const int weight, const bool italic) {
  auto face = static_cast<FT_Face>(face_);
  applyVariation(weight, italic);
  size26_6_ = 0;
  if (!ensureSize26_6(uint32_t(sizePx) * 64u)) {
    FT_Done_Face(face);
    face_ = nullptr;
    return false;
  }
  ready_ = true;
  return true;
}

void FtFont::applyVariation(const int weight, const bool italic) {
  auto face = static_cast<FT_Face>(face_);
  obliqueShear_ = false;
  emboldenBold_ = false;
  const bool wantBold = weight >= 600;
  FT_MM_Var* mm = nullptr;
  if (FT_Get_MM_Var(face, &mm) != 0 || mm == nullptr) {
    // Static face: no axes. Italic → oblique shear; bold → per-glyph outline
    // embolden (see rasterize). A separate Bold/Italic file, when the caller
    // supplies one, is loaded at weight 400 upright so neither synthesis fires.
    obliqueShear_ = italic;
    emboldenBold_ = wantBold;
  } else {
    FT_Fixed coords[16];
    const FT_UInt n = mm->num_axis < 16 ? mm->num_axis : 16;
    bool haveItalAxis = false;
    bool haveWghtAxis = false;
    for (FT_UInt i = 0; i < n; ++i) {
      coords[i] = mm->axis[i].def;
      const FT_ULong tag = mm->axis[i].tag;
      if (tag == kTagWght) {
        FT_Fixed w = static_cast<FT_Fixed>(weight) * 65536;
        if (w < mm->axis[i].minimum) w = mm->axis[i].minimum;
        if (w > mm->axis[i].maximum) w = mm->axis[i].maximum;
        coords[i] = w;
        haveWghtAxis = true;
      } else if (italic && tag == kTagItal) {
        coords[i] = mm->axis[i].maximum;  // ital 0..1 → 1
        haveItalAxis = true;
      } else if (italic && tag == kTagSlnt) {
        coords[i] = mm->axis[i].minimum;  // slnt negative = slanted
        haveItalAxis = true;
      }
    }
    FT_Set_Var_Design_Coordinates(face, n, coords);
    FT_Done_MM_Var(g_lib, mm);
    obliqueShear_ = italic && !haveItalAxis;
    emboldenBold_ = wantBold && !haveWghtAxis;  // variable but no wght axis
  }
}

bool FtFont::setRenderOptions(const RenderOptions& options) {
  options_ = options;
  // Report (without refusing) a request this build can't honor, so a caller
  // with real logging can warn instead of silently getting degraded output —
  // e.g. monochrome with FREEINK_FONT_ENABLE_MONOCHROME off fails every
  // FT_Render_Glyph call and rasterize() returns nullptr for every glyph.
  // Also validated here rather than left to FreeType: an out-of-range
  // interpreterVersion would otherwise fail FT_Property_Set silently deep
  // inside a hot per-glyph path with no way for the caller to find out.
  bool supported = true;
  if (options.monochrome && !kMonochromeCompiled) supported = false;
  if ((options.hinting == HintingMode::Auto || options.hinting == HintingMode::Light) && !kAutohintCompiled)
    supported = false;
  if (options.hinting == HintingMode::Native) {
    if (!kNativeHintingCompiled) supported = false;
    if (options.interpreterVersion != 35 && options.interpreterVersion != 40) supported = false;
  }
  return supported;
}

// Both FT_Property_Set targets live on the shared library, not this face —
// FT_Property_Set has no per-face scope. Applying them here, immediately
// before THIS face's own FT_Load_Char (not once back in setRenderOptions()),
// is what actually prevents cross-face leakage: setting them eagerly at
// setRenderOptions() time meant "whichever face configured itself most
// recently" won for every OTHER face's subsequent renders too, since
// FT_Load_Char merely consumes whatever the properties currently say rather
// than re-deriving them from this instance's own options_. Re-pinning them
// right before the one call that consumes them means only the face actually
// rendering right now can be the one that matters, for that one load.
void FtFont::applyGlobalProperties() const {
  if (!ensureLib()) return;
  FT_Bool noStemDarkening = options_.stemDarkening ? 0 : 1;
  FT_Property_Set(g_lib, "autofitter", "no-stem-darkening", &noStemDarkening);
#if FREEINK_FONT_ENABLE_NATIVE_HINTING
  // The TrueType interpreter version is library-global. Restore it before
  // every load so a face rendered with Native v35 cannot affect a later
  // Default/Auto/None face (Default uses FreeType's normal v40 behavior).
  const FT_UInt version = options_.hinting == HintingMode::Native ? options_.interpreterVersion : 40;
  FT_Property_Set(g_lib, "truetype", "interpreter-version", &version);
#endif
}

void FtFont::freeMonoBuffer() {
  fontFree(monoBuf_);
  monoBuf_ = nullptr;
  monoBufCap_ = 0;
}

const uint8_t* FtFont::expandMonoCoverage(const void* ftBitmapPtr) {
  const auto& bitmap = *static_cast<const FT_Bitmap*>(ftBitmapPtr);
  const size_t pixels = size_t(bitmap.width) * bitmap.rows;
  if (pixels == 0) return monoBuf_;  // empty glyph (e.g. space): nothing to expand
  if (pixels > monoBufCap_) {
    fontFree(monoBuf_);
    monoBuf_ = static_cast<uint8_t*>(fontAlloc(pixels));
    monoBufCap_ = monoBuf_ ? pixels : 0;
    if (!monoBuf_) return nullptr;
  }
  const int pitch = bitmap.pitch;
  for (unsigned y = 0; y < bitmap.rows; ++y) {
    const uint8_t* row =
        pitch >= 0 ? bitmap.buffer + size_t(y) * pitch : bitmap.buffer + size_t(bitmap.rows - 1 - y) * size_t(-pitch);
    uint8_t* dst = monoBuf_ + size_t(y) * bitmap.width;
    for (unsigned x = 0; x < bitmap.width; ++x) {
      dst[x] = (row[x / 8] & (0x80u >> (x % 8))) ? 0xFF : 0x00;
    }
  }
  return monoBuf_;
}

bool FtFont::ensureSize26_6(const uint32_t pixelSize26_6) {
  if (!face_ || pixelSize26_6 < 64 || pixelSize26_6 > 0xFFFFFFu) return false;
  if (pixelSize26_6 == size26_6_) return true;
  if (FT_Set_Char_Size(static_cast<FT_Face>(face_), pixelSize26_6, pixelSize26_6, 72, 72) != 0) return false;
  size26_6_ = pixelSize26_6;
  return true;
}

bool FtFont::prepareLoad() {
  if (!ready_) return false;
  applyGlobalProperties();
  const int64_t syntheticItalic = obliqueShear_ ? 13763 : 0;  // tan(about 12 degrees) in 16.16
  const FT_Fixed shear =
      static_cast<FT_Fixed>(std::clamp<int64_t>(syntheticItalic + options_.slant16_16, INT32_MIN, INT32_MAX));
  FT_Matrix matrix{0x10000L, shear, 0, 0x10000L};
  FT_Set_Transform(static_cast<FT_Face>(face_), shear ? &matrix : nullptr, nullptr);
  return true;
}

bool FtFont::loadGlyph(const GlyphId glyph, const uint32_t pixelSize26_6) {
  if (!glyph || !ensureSize26_6(pixelSize26_6) || !prepareLoad()) return false;
  auto face = static_cast<FT_Face>(face_);
  if (FT_Load_Glyph(face, glyph, loadFlagsFor(options_)) != 0) return false;
  FT_GlyphSlot slot = face->glyph;
  const int64_t syntheticBold = emboldenBold_ ? int64_t(pixelSize26_6) / 26 : 0;
  const FT_Pos strength =
      static_cast<FT_Pos>(std::clamp<int64_t>(syntheticBold + options_.embolden26_6, INT32_MIN, INT32_MAX));
  return !strength || slot->format != FT_GLYPH_FORMAT_OUTLINE || FT_Outline_Embolden(&slot->outline, strength) == 0;
}

bool FtFont::hasGlyph(const uint32_t codepoint) const { return glyphId(codepoint) != 0; }

FtFont::GlyphId FtFont::glyphId(const uint32_t codepoint) const {
  return ready_ ? FT_Get_Char_Index(static_cast<FT_Face>(face_), codepoint) : 0;
}

bool FtFont::metrics26_6(const uint32_t codepoint, const uint32_t pixelSize26_6, GlyphMetrics& out) {
  return metricsGlyph26_6(glyphId(codepoint), pixelSize26_6, out);
}

bool FtFont::metricsGlyph26_6(const GlyphId glyph, const uint32_t pixelSize26_6, GlyphMetrics& out) {
  out = GlyphMetrics{};
  if (!loadGlyph(glyph, pixelSize26_6)) return false;
  auto slot = static_cast<FT_Face>(face_)->glyph;
  long left = 0;
  long right = 0;
  long bottom = 0;
  long top = 0;
  if (options_.monochrome || slot->format != FT_GLYPH_FORMAT_OUTLINE) {
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, renderModeFor(options_)) != 0) return false;
    left = slot->bitmap_left;
    right = left + slot->bitmap.width;
    top = slot->bitmap_top;
    bottom = top - slot->bitmap.rows;
  } else {
    FT_BBox box;
    FT_Outline_Get_CBox(&slot->outline, &box);
    left = box.xMin >> 6;
    right = (box.xMax + 63) >> 6;
    bottom = box.yMin >> 6;
    top = (box.yMax + 63) >> 6;
  }
  if (right < left || top < bottom || right - left > UINT16_MAX || top - bottom > UINT16_MAX || left < INT16_MIN ||
      left > INT16_MAX || top < INT16_MIN || top > INT16_MAX)
    return false;
  out = {fixed16_16To26_6(slot->linearHoriAdvance), int16_t(left), int16_t(top), uint16_t(right - left),
         uint16_t(top - bottom)};
  return true;
}

int32_t FtFont::kerning26_6(const uint32_t left, const uint32_t right, const uint32_t pixelSize26_6) {
  return kerningGlyphs26_6(glyphId(left), glyphId(right), pixelSize26_6);
}

int32_t FtFont::kerningGlyphs26_6(const GlyphId left, const GlyphId right, const uint32_t pixelSize26_6) {
  if (!left || !right || !ensureSize26_6(pixelSize26_6)) return 0;
  auto face = static_cast<FT_Face>(face_);
  if (FT_HAS_KERNING(face)) {
    FT_Vector value{};
    if (FT_Get_Kerning(face, left, right, FT_KERNING_UNFITTED, &value) == 0 && value.x != 0) {
      return int32_t(std::clamp<int64_t>(value.x, INT32_MIN, INT32_MAX));
    }
  }
  // GPOS fallback: modern fonts carry pair kerning only in the GPOS 'kern'
  // feature, which FT_Get_Kerning (legacy 'kern' table only) cannot see.
  // The parser returns font units; x_scale converts to 26.6 at this size.
  ensureGposLoaded();
  if (!gposTable_) return 0;
  const int32_t funits = gpos::PairKernAdjustment(gposTable_, gposTableSize_, left, right);
  if (funits == 0) return 0;
  return int32_t(std::clamp<FT_Long>(FT_MulFix(funits, face->size->metrics.x_scale), INT32_MIN, INT32_MAX));
}

bool FtFont::lineMetrics26_6(const uint32_t pixelSize26_6, LineMetrics& out) {
  out = LineMetrics{};
  if (!ensureSize26_6(pixelSize26_6)) return false;
  const auto& metrics = static_cast<FT_Face>(face_)->size->metrics;
  out = {int32_t(std::clamp<int64_t>(metrics.ascender, INT32_MIN, INT32_MAX)),
         int32_t(std::clamp<int64_t>(metrics.descender, INT32_MIN, INT32_MAX)),
         int32_t(std::clamp<int64_t>(metrics.height, INT32_MIN, INT32_MAX))};
  return true;
}

int16_t FtFont::advance(const uint32_t codepoint, const uint16_t sizePx, uint8_t) {
  GlyphMetrics metrics;
  if (!metrics26_6(codepoint, uint32_t(sizePx) * 64u, metrics)) return 0;
  return int16_t(std::clamp<int32_t>(metrics.advance26_6 >> 6, INT16_MIN, INT16_MAX));
}

int16_t FtFont::lineHeight(const uint16_t sizePx) {
  LineMetrics metrics;
  return lineMetrics26_6(uint32_t(sizePx) * 64u, metrics)
             ? int16_t(std::clamp<int32_t>(metrics.height26_6 >> 6, INT16_MIN, INT16_MAX))
             : int16_t(sizePx);
}

int16_t FtFont::ascent(const uint16_t sizePx) {
  LineMetrics metrics;
  return lineMetrics26_6(uint32_t(sizePx) * 64u, metrics)
             ? int16_t(std::clamp<int32_t>(metrics.ascender26_6 >> 6, INT16_MIN, INT16_MAX))
             : int16_t(sizePx);
}

int16_t FtFont::kerning(const uint32_t left, const uint32_t right, const uint16_t sizePx, uint8_t) {
  if (!ready_) return 0;
  // Route through the glyph-ID form so the integer-pixel Font API sees the
  // GPOS fallback too; round the 26.6 result to whole pixels.
  const int32_t value = kerningGlyphs26_6(glyphId(left), glyphId(right), uint32_t(sizePx) * 64u);
  return int16_t(std::clamp<int32_t>((value + 32) >> 6, INT16_MIN, INT16_MAX));
}

void FtFont::ensureGsubLoaded() {
  // Only remember a real attempt: setting the flag before `ready_` is
  // confirmed would permanently skip loading if this got called too early
  // (e.g. a caller probing ligature() before init() succeeded), with no way
  // to retry once the face actually becomes ready.
  if (!ready_ || gsubLoadAttempted_) return;
  gsubLoadAttempted_ = true;

  // Memory-backed faces can inspect the caller-owned sfnt directory directly.
  // This keeps the GSUB table shared across all styled faces over one resident
  // font instead of allocating a duplicate copy.
  if (fontData_ != nullptr) {
    const uint8_t* table = nullptr;
    size_t tableSize = 0;
    if (findSfntTable(fontData_, fontDataSize_, kTagGsub, &table, &tableSize) && tableSize <= kMaxGsubBytes) {
      gsubTable_ = table;
      gsubTableSize_ = tableSize;
    }
    return;
  }

  auto face = static_cast<FT_Face>(face_);
  FT_ULong length = 0;
  // First call with a null buffer just reports the table's length.
  if (FT_Load_Sfnt_Table(face, kTagGsub, 0, nullptr, &length) != 0 || length == 0 || length > kMaxGsubBytes ||
      length > gsubByteBudget_) {
    return;
  }
  // Fallible allocation, deliberately not PsramVector (see the header
  // comment): a missing GSUB table just means no ligatures, not a crash.
  auto* buffer = static_cast<uint8_t*>(fontAlloc(length));
  if (!buffer) return;
  if (FT_Load_Sfnt_Table(face, kTagGsub, 0, buffer, &length) != 0) {
    fontFree(buffer);
    return;
  }
  gsubTable_ = buffer;
  gsubTableSize_ = length;
  gsubTableOwned_ = true;
}

void FtFont::ensureGposLoaded() {
  // Mirrors ensureGsubLoaded(), including the ready_-before-flag ordering:
  // a probe before init() must stay retryable once the face is live.
  if (!ready_ || gposLoadAttempted_) return;
  gposLoadAttempted_ = true;

  if (fontData_ != nullptr) {
    const uint8_t* table = nullptr;
    size_t tableSize = 0;
    if (findSfntTable(fontData_, fontDataSize_, kTagGpos, &table, &tableSize) && tableSize <= kMaxGsubBytes) {
      gposTable_ = table;
      gposTableSize_ = tableSize;
    }
    return;
  }

  auto face = static_cast<FT_Face>(face_);
  FT_ULong length = 0;
  if (FT_Load_Sfnt_Table(face, kTagGpos, 0, nullptr, &length) != 0 || length == 0 || length > kMaxGsubBytes ||
      length > gposByteBudget_) {
    return;
  }
  // Fallible allocation, same policy as the GSUB copy: a missing GPOS table
  // just means no pair kerning, not a crash.
  auto* buffer = static_cast<uint8_t*>(fontAlloc(length));
  if (!buffer) return;
  if (FT_Load_Sfnt_Table(face, kTagGpos, 0, buffer, &length) != 0) {
    fontFree(buffer);
    return;
  }
  gposTable_ = buffer;
  gposTableSize_ = length;
  gposTableOwned_ = true;
}

uint32_t FtFont::ligatureGlyphId(const uint32_t* codepoints, const unsigned length) {
  if (!ready_ || codepoints == nullptr || length < 2 || length > 3) return 0;
  ensureGsubLoaded();
  if (!gsubTable_) return 0;
  auto face = static_cast<FT_Face>(face_);
  uint32_t glyphs[3];
  for (unsigned i = 0; i < length; ++i) {
    const FT_UInt g = FT_Get_Char_Index(face, codepoints[i]);
    if (g == 0) return 0;
    glyphs[i] = g;
  }
  return gsub::LigatureGlyphId(gsubTable_, gsubTableSize_, glyphs, length);
}

uint32_t FtFont::ligature(const uint32_t left, const uint32_t right, uint8_t) {
  if (!ready_) return 0;
  auto face = static_cast<FT_Face>(face_);

  // Font::ligature() only ever sees a pair, chaining through a previous
  // result for a 3-glyph ligature ("ff"+"i" using the U+FB00 "ff" result as
  // the new left) — see Font.h. A real GSUB table keys "ffi" on the original
  // 3 letters (f, f, i), not on the already-substituted "ff" glyph, so a
  // chained call is rewritten back to its 3 original codepoints before
  // asking GSUB, instead of querying GSUB for a rule no font actually has.
  uint32_t sequence[3];
  unsigned length;
  uint32_t candidates[2];
  unsigned candidateCount;
  if (left == 0xFB00 && (right == 'i' || right == 'l')) {  // previous step already found "ff"
    sequence[0] = 'f';
    sequence[1] = 'f';
    sequence[2] = right;
    length = 3;
    candidates[0] = right == 'i' ? 0xFB03 : 0xFB04;  // ffi / ffl
    candidateCount = 2;
  } else if (left == 'f' && (right == 'f' || right == 'i' || right == 'l')) {
    sequence[0] = left;
    sequence[1] = right;
    length = 2;
    candidates[1] = 0;
    candidateCount = 1;
    candidates[0] = right == 'f' ? 0xFB00 : right == 'i' ? 0xFB01 : 0xFB02;  // ff / fi / fl
  } else {
    // Not one of the five pairs this bridges to a codepoint at all — return
    // early instead of asking GSUB a question whose answer (if any) could
    // never be expressed through this contract anyway, and instead of
    // defaulting `candidates[0]` to 0xFB00, which would falsely credit an
    // unrelated pair's real GSUB match to the "ff" ligature codepoint.
    return 0;
  }

  const uint32_t ligatureGlyph = ligatureGlyphId(sequence, length);
  if (ligatureGlyph == 0) return 0;

  // Font::ligature() must return a real Unicode codepoint — the layout pass
  // bakes it into cached page text — but GSUB substitution glyphs commonly
  // have no cmap entry at all. Confirm one of the standard Latin ligature
  // codepoints actually maps to the glyph GSUB just named; if none does,
  // there is nothing this contract can hand back even though GSUB matched.
  // A glyph-indexed consumer that doesn't need a codepoint can call
  // ligatureGlyphId() directly instead and use `ligatureGlyph` as-is.
  for (unsigned i = 0; i < candidateCount; ++i) {
    if (FT_Get_Char_Index(face, candidates[i]) == ligatureGlyph) return candidates[i];
  }
  return 0;
}

const GlyphBitmap* FtFont::rasterize(const uint32_t codepoint, const uint16_t sizePx) {
  return rasterize26_6(codepoint, uint32_t(sizePx) * 64u);
}

const GlyphBitmap* FtFont::rasterize26_6(const uint32_t codepoint, const uint32_t pixelSize26_6) {
  return rasterizeGlyph26_6(glyphId(codepoint), pixelSize26_6);
}

const GlyphBitmap* FtFont::rasterizeGlyph26_6(const GlyphId glyph, const uint32_t pixelSize26_6) {
  if (!loadGlyph(glyph, pixelSize26_6)) return nullptr;
  FT_GlyphSlot s = static_cast<FT_Face>(face_)->glyph;
  if (s->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(s, renderModeFor(options_)) != 0) return nullptr;
  if (s->bitmap.width > UINT16_MAX || s->bitmap.rows > UINT16_MAX || s->bitmap_left < INT16_MIN ||
      s->bitmap_left > INT16_MAX || s->bitmap_top < INT16_MIN || s->bitmap_top > INT16_MAX ||
      (s->advance.x >> 6) < INT16_MIN || (s->advance.x >> 6) > INT16_MAX)
    return nullptr;
  // GlyphBitmap's contract is 8-bit coverage (see Font.h); FT_PIXEL_MODE_MONO
  // is 1-bpp packed and must be expanded, not published as-is.
  if (s->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
    const uint8_t* expanded = expandMonoCoverage(&s->bitmap);
    if (!expanded && s->bitmap.width && s->bitmap.rows) return nullptr;  // allocation failure
    glyph_.pixels = expanded;
  } else {
    glyph_.pixels = s->bitmap.buffer;  // 8-bit alpha; valid until next load
  }
  glyph_.width = static_cast<uint16_t>(s->bitmap.width);
  glyph_.height = static_cast<uint16_t>(s->bitmap.rows);
  glyph_.xoff = static_cast<int16_t>(s->bitmap_left);
  glyph_.yoff = static_cast<int16_t>(-s->bitmap_top);  // top offset from baseline, negative = above
  glyph_.advance = static_cast<int16_t>(s->advance.x >> 6);
  return &glyph_;
}

}  // namespace font
}  // namespace freeink
