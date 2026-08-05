#include "ImageToneMapper.h"

#include <cstring>
#include <new>

namespace freeink {

namespace {
// Free a typed array allocated with new[] and null the pointer.
template <typename T> void dropArray(T *&p) {
  delete[] p;
  p = nullptr;
}
} // namespace

ImageToneMapper::~ImageToneMapper() { reset(); }

void ImageToneMapper::reset() {
  dropArray(colMap_);
  dropArray(accSum_);
  dropArray(accCnt_);
  dropArray(gray8_);
  dropArray(lastGray8_);
  dropArray(outLevels_);
  dropArray(errCur_);
  dropArray(errNext_);
  srcWidth_ = srcHeight_ = dstWidth_ = dstHeight_ = 0;
  sink_ = nullptr;
}

bool ImageToneMapper::begin(int srcWidth, int srcHeight, int dstWidth,
                            int dstHeight, int numLevels, RowSink sink,
                            void *sinkCtx) {
  reset();

  if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0 ||
      numLevels < 2 || numLevels > 16 || !sink) {
    return false;
  }

  srcWidth_ = srcWidth;
  srcHeight_ = srcHeight;
  dstWidth_ = dstWidth;
  dstHeight_ = dstHeight;
  numLevels_ = numLevels;
  step_ = 255 / (numLevels - 1);
  sink_ = sink;
  sinkCtx_ = sinkCtx;
  curSrcY_ = 0;
  accDstY_ = 0;
  rowSeen_ = false;

  // Allocate all working buffers. new (std::nothrow) T[] returns nullptr on OOM
  // (never throws) so every allocation is null-checked before use.
  colMap_ = new (std::nothrow) uint16_t[srcWidth_];
  accSum_ = new (std::nothrow) uint32_t[dstWidth_];
  accCnt_ = new (std::nothrow) uint16_t[dstWidth_];
  gray8_ = new (std::nothrow) uint8_t[dstWidth_];
  lastGray8_ = new (std::nothrow) uint8_t[dstWidth_];
  outLevels_ = new (std::nothrow) uint8_t[dstWidth_];
  errCur_ = new (std::nothrow) int32_t[dstWidth_ + 2];
  errNext_ = new (std::nothrow) int32_t[dstWidth_ + 2];

  if (!colMap_ || !accSum_ || !accCnt_ || !gray8_ || !lastGray8_ ||
      !outLevels_ || !errCur_ || !errNext_) {
    reset();
    return false;
  }

  // Area-average column binning: every source column maps to exactly one
  // destination column, so accumulating src[sx] into colMap_[sx] and dividing
  // by the sample count yields a box-filter average. On horizontal upscale some
  // destination columns receive no source column; those gaps are filled from
  // the left neighbour when the row is finalised.
  for (int sx = 0; sx < srcWidth_; ++sx) {
    int dx =
        static_cast<int>((static_cast<int64_t>(sx) * dstWidth_) / srcWidth_);
    if (dx >= dstWidth_)
      dx = dstWidth_ - 1;
    colMap_[sx] = static_cast<uint16_t>(dx);
  }

  memset(accSum_, 0, sizeof(uint32_t) * dstWidth_);
  memset(accCnt_, 0, sizeof(uint16_t) * dstWidth_);
  memset(lastGray8_, 0, dstWidth_);
  memset(errCur_, 0, sizeof(int32_t) * (dstWidth_ + 2));
  memset(errNext_, 0, sizeof(int32_t) * (dstWidth_ + 2));
  return true;
}

void ImageToneMapper::addSourceRow(const uint8_t *srcRow) {
  if (!sink_ || curSrcY_ >= srcHeight_)
    return;

  // Which destination row does this source row belong to? On downscale several
  // consecutive source rows share a dstY (accumulate); on upscale dstY can jump
  // by more than one (the skipped rows are replicated at finalise time).
  int dstY = static_cast<int>((static_cast<int64_t>(curSrcY_) * dstHeight_) /
                              srcHeight_);
  if (dstY >= dstHeight_)
    dstY = dstHeight_ - 1;

  // The accumulation for accDstY_ (and any upscale-gap rows before dstY) is now
  // final; flush them before starting the new destination row.
  while (accDstY_ < dstY) {
    finalizeRow(accDstY_);
    ++accDstY_;
  }

  // Accumulate this source row into the current destination row.
  for (int sx = 0; sx < srcWidth_; ++sx) {
    const int dx = colMap_[sx];
    accSum_[dx] += srcRow[sx];
    accCnt_[dx] += 1;
  }
  rowSeen_ = true;
  ++curSrcY_;
}

void ImageToneMapper::finish() {
  if (!sink_)
    return;
  // Flush the last accumulated row and any trailing rows (upscale tail).
  while (accDstY_ < dstHeight_) {
    finalizeRow(accDstY_);
    ++accDstY_;
  }
}

void ImageToneMapper::finalizeRow(int dstY) {
  // 1. Build the 8-bit averaged row. A destination column with no samples this
  //    row is a gap: horizontally (upscale) fill from the left neighbour;
  //    vertically (an entirely empty row) replicate the previous averaged row.
  if (rowSeen_) {
    uint8_t left = lastGray8_[0];
    for (int dx = 0; dx < dstWidth_; ++dx) {
      if (accCnt_[dx] > 0) {
        left = static_cast<uint8_t>(accSum_[dx] / accCnt_[dx]);
      }
      gray8_[dx] = left;
    }
  } else {
    memcpy(gray8_, lastGray8_, dstWidth_);
  }

  // 2. Floyd-Steinberg across the row, left to right. errCur_/errNext_ use a +1
  //    index offset so the x-1 and x+1 spills at the edges stay in bounds.
  const int maxLevel = numLevels_ - 1;
  for (int dx = 0; dx < dstWidth_; ++dx) {
    int v = gray8_[dx] + errCur_[dx + 1];
    if (v < 0)
      v = 0;
    if (v > 255)
      v = 255;

    int q = (v + step_ / 2) / step_;
    if (q < 0)
      q = 0;
    if (q > maxLevel)
      q = maxLevel;
    outLevels_[dx] = static_cast<uint8_t>(q);

    const int err = v - q * step_;
    // Standard Floyd-Steinberg weights: 7/16 right, 3/16 down-left, 5/16 down,
    // 1/16 down-right.
    errCur_[dx + 2] += err * 7 / 16;
    errNext_[dx] += err * 3 / 16;
    errNext_[dx + 1] += err * 5 / 16;
    errNext_[dx + 2] += err * 1 / 16;
  }

  sink_(sinkCtx_, dstY, outLevels_, dstWidth_);

  // 3. Roll error buffers forward and reset per-row state.
  memcpy(lastGray8_, gray8_, dstWidth_);
  int32_t *tmp = errCur_;
  errCur_ = errNext_;
  errNext_ = tmp;
  memset(errNext_, 0, sizeof(int32_t) * (dstWidth_ + 2));
  memset(accSum_, 0, sizeof(uint32_t) * dstWidth_);
  memset(accCnt_, 0, sizeof(uint16_t) * dstWidth_);
  rowSeen_ = false;
}

} // namespace freeink
