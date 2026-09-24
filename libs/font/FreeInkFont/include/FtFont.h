#pragma once

// FreeInk SDK — FreeType-backed font engine (FreeInkFont).
//
// FtFont implements RasterFont over FreeType. Unlike the stb_truetype backend
// (TtfFont), it reads OpenType variable-font axes, so a single variable file
// yields real weights (bold = wght axis) and, where present, a real italic/slant
// axis — and it can synthesize oblique italic and emboldening when an axis is
// absent. It also streams large CJK faces (FreeType maps tables on demand), which
// stb_truetype cannot on a no-PSRAM device.
//
// One FtFont instance = one styled face at one pixel size: pick the design
// `weight` (e.g. 400 regular / 700 bold) and `italic` at init. A font FAMILY is
// several FtFonts over the SAME borrowed file bytes at different coordinates,
// surfaced to a renderer as regular/bold/italic/bold-italic.
//
// Memory: the font file bytes are BORROWED (PSRAM / resident buffer) and must
// outlive the FtFont. FreeType owns the glyph slot, so rasterize() returns a
// bitmap valid until the next rasterize() on this face (the RasterFont contract);
// no external glyph arena is needed. FreeType's own allocations are routed to
// PSRAM (when present) via a custom FT_Memory — see ensureLib() in FtFont.cpp
// and FontAlloc.h.

#include <stddef.h>
#include <stdint.h>

#include "Font.h"

// Opaque FreeType handles kept out of the public header.
typedef struct FT_LibraryRec_* FtLibraryHandle;
typedef struct FT_FaceRec_* FtFaceHandle;

namespace freeink {
namespace font {

class FtFont : public RasterFont {
 public:
  using GlyphId = uint32_t;

  // FreeType-native fixed-point values. Pixel sizes and advances use 26.6
  // units (64 == one pixel); slant uses a 16.16 shear coefficient. Keeping
  // these units here lets applications apply their own physical-display
  // policy without the SDK assuming a particular DPI.
  struct GlyphMetrics {
    int32_t advance26_6 = 0;
    int16_t left = 0;
    int16_t top = 0;
    uint16_t width = 0;
    uint16_t height = 0;
  };
  struct LineMetrics {
    int32_t ascender26_6 = 0;
    int32_t descender26_6 = 0;
    int32_t height26_6 = 0;
  };
  struct FaceInfo {
    size_t familyLength = 0;
    uint16_t weight = 400;
    bool italic = false;
  };
  enum class InspectResult : int8_t { Ok = 0, Unsupported = -1, Unavailable = -2 };

  // Optional process-wide allocator for the shared FreeType library and
  // FtFont-owned scratch buffers. Configure it before the first FtFont init or
  // inspection. Passing nullptr selects the platform default allocator.
  // Reallocation receives both sizes so bounded arenas do not need a hidden
  // allocation header. Returns false after the shared library has started or
  // when the callback set is incomplete.
  struct MemoryCallbacks {
    void* context = nullptr;
    void* (*allocate)(void* context, size_t size) = nullptr;
    void (*deallocate)(void* context, void* block) = nullptr;
    void* (*reallocate)(void* context, void* block, size_t oldSize, size_t newSize) = nullptr;
  };
  static bool configureMemory(const MemoryCallbacks* callbacks);

  FtFont() = default;
  ~FtFont();
  FtFont(const FtFont&) = delete;
  FtFont& operator=(const FtFont&) = delete;

  // Borrow `data` (must outlive this face and any use of the face). `weight` is
  // the design weight applied to the font's `wght` axis if it has one (clamped
  // to the axis range); ignored
  // on a static face. `italic`: use a real ital/slnt axis if present, else apply
  // an oblique shear. Returns false if FreeType can't parse the face.
  bool init(const uint8_t* data, uint32_t len, uint16_t sizePx, int weight = 400, bool italic = false);

  // Streamed variant: instead of holding the whole file in RAM, FreeType pulls
  // bytes on demand through `read` (absolute offset). `fileSize` is the total
  // length; `ctx` is passed back to `read` and, along with whatever it wraps
  // (e.g. an open SD file), must outlive this face. This keeps only the tables
  // and glyphs actually used resident — essential for multi-MB variable/CJK
  // fonts. The source is NOT closed on destruction (the caller owns it). A
  // streamed face copies its GSUB table lazily when ligatureGlyphId() first
  // needs it; setGsubByteBudget() can bound that copy.
  using ReadFn = unsigned long (*)(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
  bool initStream(ReadFn read, void* ctx, unsigned long fileSize, uint16_t sizePx, int weight = 400,
                  bool italic = false);

  bool ready() const { return ready_; }

  // Read face metadata without retaining a face. The family buffer is
  // optional and always NUL-terminated when familyCapacity is nonzero.
  static InspectResult inspectMemory(const uint8_t* data, uint32_t length, FaceInfo& info, char* family = nullptr,
                                     size_t familyCapacity = 0);
  static InspectResult inspectStream(ReadFn read, void* ctx, unsigned long fileSize, FaceInfo& info,
                                     char* family = nullptr, size_t familyCapacity = 0);

  // Hinting/rasterization tuning, independent of the style axes chosen at
  // init() time. Default blocks auto-hint fallback while retaining FreeType's
  // normal native-hinting behavior (when that module is compiled); None is the
  // explicit no-hinting mode. Native only selects native hints when built with
  // FREEINK_FONT_ENABLE_NATIVE_HINTING; otherwise it reports unsupported and
  // degrades according to the requested load flags. interpreterVersion and
  // stemDarkening are properties of the shared FreeType library, not one face
  // (FT_Property_Set has no per-face scope), so they are applied immediately
  // before each glyph load from this face's options. setRenderOptions() only
  // stores the options and reports whether the requested modules are present.
  enum class HintingMode : uint8_t { Default = 0, None, Auto, Light, Native };
  struct RenderOptions {
    HintingMode hinting = HintingMode::Default;
    uint8_t interpreterVersion = 40;  // 35 or 40; only meaningful with HintingMode::Native
    bool monochrome = false;          // 1-bit coverage instead of 8-bit grayscale
    bool stemDarkening = false;       // auto-hinter stem darkening; matches FreeType's own default
    int32_t embolden26_6 = 0;         // outline strength; independent of design-axis weight
    int32_t slant16_16 = 0;           // additional x shear; 0x10000 == 45 degrees
  };
  // Returns false (without refusing the call — options_ is still stored) when
  // the request needs a module this build didn't compile in, so a caller
  // with real logging can warn instead of silently getting degraded output.
  bool setRenderOptions(const RenderOptions& options);

  // Release the FreeType face (and any streamed source wrappers), returning the
  // object to the pre-init state. The borrowed file bytes / stream source are
  // NOT freed (the caller owns them). Safe to init()/initStream() again after —
  // lets a caller shed an idle face's FreeType memory and rebuild it on demand.
  void deinit();

  bool hasGlyph(uint32_t codepoint) const override;

  // Generic low-level access for renderers that cache glyph IDs or need
  // fractional pixel sizes. A zero glyph ID is always missing. Bitmap data is
  // 8-bit coverage and remains valid until the next glyph load on this face.
  //
  // Kerning consults the legacy 'kern' table first (FT_Get_Kerning), then
  // falls back to the GPOS 'kern' feature's pair-adjustment lookups (see
  // Gpos.h) — modern fonts ship pair kerning exclusively there, which
  // FreeType alone cannot see. The GPOS table stays cached for the face's
  // lifetime (queries arrive per adjacent pair at render time); a streamed
  // face's owned copy is bounded by setGposByteBudget().
  GlyphId glyphId(uint32_t codepoint) const;
  bool metrics26_6(uint32_t codepoint, uint32_t pixelSize26_6, GlyphMetrics& out);
  bool metricsGlyph26_6(GlyphId glyph, uint32_t pixelSize26_6, GlyphMetrics& out);
  int32_t kerning26_6(uint32_t left, uint32_t right, uint32_t pixelSize26_6);
  int32_t kerningGlyphs26_6(GlyphId left, GlyphId right, uint32_t pixelSize26_6);
  bool lineMetrics26_6(uint32_t pixelSize26_6, LineMetrics& out);
  const GlyphBitmap* rasterize26_6(uint32_t codepoint, uint32_t pixelSize26_6);
  const GlyphBitmap* rasterizeGlyph26_6(GlyphId glyph, uint32_t pixelSize26_6);

  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override;
  int16_t lineHeight(uint16_t sizePx) override;
  int16_t ascent(uint16_t sizePx) override;
  int16_t kerning(uint32_t left, uint32_t right, uint16_t sizePx, uint8_t styleFlags) override;

  // Standard Latin ligatures (fi fl ff ffi ffl), same coverage as TtfFont's,
  // but resolved via the face's actual GSUB 'liga'/'rlig' table (see Gsub.h)
  // instead of a hardcoded ASCII-pair guess — so it also catches fonts whose
  // ligature setup differs from the naive check (e.g. a feature gated on
  // something GSUB-visible that a plain pair match would miss). Still bounded
  // by the Font::ligature() contract: it can only return a real Unicode
  // codepoint, so a font whose GSUB target glyph has no cmap entry (the
  // common case for professionally-authored fonts) yields 0 here even though
  // GSUB does have a substitution — see ligatureGlyphId() below for the
  // unrestricted form a glyph-indexed consumer can use directly.
  uint32_t ligature(uint32_t left, uint32_t right, uint8_t styleFlags) override;

  // The raw GSUB-substituted glyph ID for a 2-3 glyph sequence (glyph IDs, not
  // codepoints), or 0 when none applies. For a consumer whose own glyph cache
  // is glyph-indexed rather than codepoint-indexed and so isn't bound by
  // ligature()'s "must be a real codepoint" contract. `codepoints` and
  // `length` (2 or 3) name Unicode input; this maps them to glyph IDs via
  // this face's cmap before consulting GSUB.
  uint32_t ligatureGlyphId(const uint32_t* codepoints, unsigned length);

  // Limit the maximum streamed GSUB allocation. The default is 1 MiB for
  // compatibility with large real-world fonts. The limit applies to owned
  // tables loaded through initStream(); memory-backed init() uses a borrowed
  // view into the caller's font bytes and does not allocate. A rejected or
  // failed load simply disables GSUB ligatures for this face.
  void setGsubByteBudget(size_t maxBytes);

  // Drop the cached GSUB view/table after the caller has resolved the glyph IDs
  // it needs. For a memory-backed face this only drops the view; for a streamed
  // face it frees the owned table. Further ligature queries return 0 until the
  // face is initialized again.
  void releaseLigatureTable();

  // GPOS analogues of the GSUB pair above, governing the kerning fallback's
  // table cache. The stream budget matters more here than for GSUB: GPOS
  // cannot be resolved once and released (kern pairs are queried throughout
  // rendering), so a streamed face's owned copy stays resident — a rejected
  // or failed load simply disables GPOS kerning (legacy 'kern' still works).
  void setGposByteBudget(size_t maxBytes);
  void releaseKerningTable();

  // Rasterizes one glyph to an 8-bit alpha GlyphBitmap. Valid until the next
  // rasterize() on this face (FreeType glyph-slot lifetime).
  const GlyphBitmap* rasterize(uint32_t codepoint, uint16_t sizePx) override;

 private:
  void applyVariation(int weight, bool italic);
  bool ensureSize26_6(uint32_t pixelSize26_6);
  bool prepareLoad();
  bool loadGlyph(GlyphId glyph, uint32_t pixelSize26_6);
  void applyGlobalProperties() const;  // FT_Property_Set, called right before each FT_Load_Glyph

  bool finishInit(uint16_t sizePx, int weight, bool italic);  // shared tail of init/initStream
  void ensureGsubLoaded();
  void ensureGposLoaded();

  // A monochrome FT_Bitmap is 1-bpp packed (pitch = (width+7)/8), but
  // GlyphBitmap's contract is 8-bit coverage at stride `width` (see Font.h).
  // Expands into monoBuf_ (grown on demand, freed in deinit()) rather than
  // exposing the packed buffer directly. Returns nullptr on allocation
  // failure — the caller (rasterize()) treats that like any other raster
  // failure and returns nullptr for the glyph instead of publishing it.
  const uint8_t* expandMonoCoverage(const void* ftBitmap);
  void freeMonoBuffer();

  FtFaceHandle face_ = nullptr;
  void* stream_ = nullptr;     // FT_StreamRec* for the streamed path (owned)
  void* streamCtx_ = nullptr;  // {ReadFn, ctx} for the streamed path (owned)
  bool ready_ = false;
  bool obliqueShear_ = false;  // faux italic (no ital/slnt axis)
  bool emboldenBold_ = false;  // faux bold (static or no wght axis); per-glyph outline embolden
  uint32_t size26_6_ = 0;
  RenderOptions options_{};
  GlyphBitmap glyph_{};  // last rasterized glyph (points into the FT slot buffer, or monoBuf_)
  uint8_t* monoBuf_ = nullptr;
  size_t monoBufCap_ = 0;

  // Memory-backed faces retain a borrowed view into their whole font and point
  // gsubTable_ at the raw GSUB bytes after a bounds-checked sfnt directory
  // lookup; styled faces over one resident font therefore share the bytes.
  // Streamed faces have no whole-font pointer, so they lazily fetch one owned
  // table via FT_Load_Sfnt_Table.
  // Owned via fiFontMalloc/fiFontFree directly (NOT PsramVector, whose
  // allocator aborts the process on OOM) because this is the one buffer in
  // the class sized by untrusted font content rather than a small fixed
  // amount, on a backend this library recommends specifically for
  // constrained-RAM boards — an allocation failure or budget rejection here
  // must degrade (no ligatures) rather than crash. The owned/borrowed state is
  // reset in deinit(), since glyph IDs are face-local and stale cached bytes
  // from a previous init() would resolve the new face's glyph IDs against
  // the wrong font entirely.
  //
  // kMaxGsubBytes remains a parse-sanity ceiling; gsubByteBudget_ is the
  // caller-controlled stream allocation budget.
  static constexpr size_t kMaxGsubBytes = 1024 * 1024;
  const uint8_t* fontData_ = nullptr;
  size_t fontDataSize_ = 0;
  const uint8_t* gsubTable_ = nullptr;
  size_t gsubTableSize_ = 0;
  bool gsubTableOwned_ = false;
  bool gsubLoadAttempted_ = false;
  size_t gsubByteBudget_ = kMaxGsubBytes;
  void freeGsubTable();

  // GPOS kerning cache — same ownership/lifetime discipline as the GSUB
  // fields above (borrowed view for memory-backed faces, budgeted owned copy
  // for streamed ones, reset in deinit() because glyph IDs are face-local).
  const uint8_t* gposTable_ = nullptr;
  size_t gposTableSize_ = 0;
  bool gposTableOwned_ = false;
  bool gposLoadAttempted_ = false;
  size_t gposByteBudget_ = kMaxGsubBytes;
  void freeGposTable();
};

}  // namespace font
}  // namespace freeink
