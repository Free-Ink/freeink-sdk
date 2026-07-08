#pragma once

// FreeInk SDK — image header probe for FreeInkBook layout.
//
// Layout needs intrinsic dimensions to reserve space for an image without
// decoding it. The probe streams just enough of the entry to find them:
// the PNG IHDR (fixed offset) or the first JPEG SOF marker (walking segment
// lengths, capped). Decoding happens later, at render time.

#include <stdint.h>

#include "BookArena.h"
#include "BookStorage.h"
#include "BookTypes.h"
#include "epub/ZipCatalog.h"

namespace freeink {
namespace book {

struct ImageInfo {
  enum class Kind : uint8_t { Unknown = 0, Png, Jpeg };
  Kind kind = Kind::Unknown;
  uint16_t width = 0;
  uint16_t height = 0;
  bool progressive = false;  // JPEG SOF2 — needs the DC-only decode path
};

// Scratch is released before returning. Returns Ok with kind=Unknown for
// formats the engine does not handle (GIF, SVG, WebP) — the caller skips the
// image rather than failing the chapter.
BookStatus probeImage(BookSource& source, const ZipEntry& entry, Arena& scratch,
                      ImageInfo* out);

}  // namespace book
}  // namespace freeink
