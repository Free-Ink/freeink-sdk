#pragma once

#include <cstddef>
#include <cstdint>

namespace freeink {

// Streaming tone mapper: turns a continuous-tone (8-bit grayscale) image into a
// low-bit-depth, error-diffused image suitable for an e-ink panel. It replaces
// the two quality-limiting steps of a naive e-ink image path:
//
//   1. Downscale by AREA AVERAGING (box filter) instead of nearest-neighbour.
//      Nearest-neighbour drops whole source rows/columns when shrinking a
//      photo, which aliases edges, breaks thin lines, and creates moire.
//      Averaging every source pixel that lands in a destination pixel preserves
//      detail and pre-smooths the signal before quantisation.
//
//   2. Quantise by FLOYD-STEINBERG ERROR DIFFUSION instead of an ordered
//   (Bayer)
//      matrix. Ordered dithering leaves a fixed cross-hatch/screen-door pattern
//      that reads as "digital"; error diffusion spreads each pixel's rounding
//      error into not-yet-drawn neighbours, producing the smooth, photographic
//      look that dedicated e-readers get.
//
// STREAMING / STATEFUL CONTRACT
// -----------------------------
// Error diffusion is inherently order-dependent: a pixel's output depends on
// error accumulated from pixels above and to its left. The mapper therefore
// requires source rows to be fed strictly top-to-bottom (addSourceRow) and
// emits destination rows strictly top-to-bottom (via the sink). It must run
// exactly once over the image, in raster order. This matches how JPEGDEC
// (raster MCU order) and PNGdec (top-to-bottom scanlines) deliver pixels, and
// how the reader consumes the result (decode once, cache the 2-bit output,
// re-read the cache for every later plane/strip pass).
//
// The mapper holds only O(dstWidth) memory (a handful of row-sized buffers),
// not the whole image, so it is safe on a fragmented ~380 KB heap.
class ImageToneMapper {
public:
  // Receives one finished destination row. `levels` holds `dstWidth` values,
  // each in [0, numLevels-1] (0 = black, numLevels-1 = white). `dstY` is the
  // 0-based output row index. Rows arrive in order, exactly once each,
  // 0..dstHeight-1.
  using RowSink = void (*)(void *ctx, int dstY, const uint8_t *levels,
                           int dstWidth);

  ImageToneMapper() = default;
  ~ImageToneMapper();

  ImageToneMapper(const ImageToneMapper &) = delete;
  ImageToneMapper &operator=(const ImageToneMapper &) = delete;

  // Allocate working buffers and precompute the source->destination column map.
  //   srcWidth/srcHeight : dimensions of the rows that will be fed (i.e. the
  //                        decoder's output size, after any built-in JPEG
  //                        scaling).
  //   dstWidth/dstHeight : final on-screen size. May be smaller (downscale, the
  //                        common case), equal, or larger (upscale) than
  //                        source.
  //   numLevels          : output levels, 2..16. Use 4 for a 2-bit e-ink panel.
  // Returns false on bad arguments or OOM; the object is unusable until a
  // successful begin().
  bool begin(int srcWidth, int srcHeight, int dstWidth, int dstHeight,
             int numLevels, RowSink sink, void *sinkCtx);

  // Feed one source row of exactly srcWidth 8-bit grayscale samples. Must be
  // called srcHeight times, in top-to-bottom order.
  void addSourceRow(const uint8_t *srcRow);

  // Emit any destination rows not yet flushed (the final partially/fully
  // accumulated row and any trailing upscale-replicated rows). Idempotent.
  void finish();

private:
  void finalizeRow(int dstY);
  void reset();

  int srcWidth_ = 0;
  int srcHeight_ = 0;
  int dstWidth_ = 0;
  int dstHeight_ = 0;
  int numLevels_ = 4;
  int step_ = 85; // 255 / (numLevels - 1), the spacing between output levels

  RowSink sink_ = nullptr;
  void *sinkCtx_ = nullptr;

  int curSrcY_ = 0;      // next source row index to be consumed
  int accDstY_ = 0;      // destination row currently being accumulated
  bool rowSeen_ = false; // any source samples accumulated into accDstY_ yet

  // Precomputed source-column -> destination-column map (area-average binning).
  uint16_t *colMap_ = nullptr; // [srcWidth]

  // Per-destination-row accumulators (reset each finalized row).
  uint32_t *accSum_ =
      nullptr; // [dstWidth] sum of source samples per dst column
  uint16_t *accCnt_ = nullptr; // [dstWidth] number of samples per dst column

  uint8_t *gray8_ = nullptr; // [dstWidth] area-averaged row, pre-diffusion
  uint8_t *lastGray8_ =
      nullptr; // [dstWidth] previous averaged row (upscale gap fill)
  uint8_t *outLevels_ =
      nullptr; // [dstWidth] quantised levels handed to the sink

  // Floyd-Steinberg error carried between rows. Sized dstWidth+2 and accessed
  // via a +1 offset so x-1..x+1 are always valid at the row edges.
  int32_t *errCur_ = nullptr;  // error to add to the row being drawn
  int32_t *errNext_ = nullptr; // error accumulating for the next row
};

} // namespace freeink
