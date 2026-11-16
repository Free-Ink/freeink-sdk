#pragma once

// Full-resolution grayscale decode of progressive (SOF2) JPEGs.
//
// Every scan refines coefficients for the whole image, so the luma DCT
// coefficients are held for the entire frame (blocks x 64 x int16, about
// 2 bytes per pixel) until the last scan; rows are emitted after that. On
// ESP32 the store is allocated from PSRAM only, so boards without PSRAM get
// OutOfMemory from begin() and can fall back to a smaller decode. Chroma is
// parsed to keep the bitstream in sync but not stored: output is luma only.

#include <stddef.h>
#include <stdint.h>

namespace freeink {

class ProgressiveJpeg {
 public:
  enum class Status : uint8_t { Ok, IoError, ParseError, Unsupported, OutOfMemory, Stopped };

  // Fill dst with up to n bytes; return the count, 0 at end of data, < 0 on error.
  using ReadFn = int32_t (*)(void* ctx, uint8_t* dst, uint32_t n);
  // One output row of `width` 8-bit luma values; return false to stop.
  using RowFn = bool (*)(void* user, uint16_t y, const uint8_t* row, uint16_t width);

  ProgressiveJpeg() = default;
  ~ProgressiveJpeg();
  ProgressiveJpeg(const ProgressiveJpeg&) = delete;
  ProgressiveJpeg& operator=(const ProgressiveJpeg&) = delete;

  // Reads the stream up to its frame header and allocates the coefficient
  // store. Unsupported for anything but a progressive Huffman JPEG.
  Status begin(ReadFn read, void* ctx);
  uint16_t width() const { return _width; }
  uint16_t height() const { return _height; }
  // Bytes begin() needs for the coefficient store of a w x h image.
  static size_t storeBytes(uint16_t w, uint16_t h, uint8_t hmax = 2, uint8_t vmax = 2);

  // Decodes the whole stream from its first byte (rewind the source after
  // begin()), then emits rows 0..height-1 in order.
  Status decode(ReadFn read, void* ctx, RowFn row, void* user);

 private:
  struct Reader;
  struct Huff;
  struct Comp;
  Status parse(Reader& r, bool headerOnly);
  void scan(Reader& r, const Comp* comps, uint8_t ns, const uint8_t* idx, const Huff& acTable, const Huff* dc,
            const uint8_t* td, uint8_t ss, uint8_t se, uint8_t ah, uint8_t al, uint16_t restartInterval,
            uint16_t mcusX, uint16_t mcusY);
  Status emit(RowFn rowFn, void* user);
  void release();

  uint16_t _width = 0;
  uint16_t _height = 0;
  uint16_t _blocksW = 0;  // luma blocks per row in the store (MCU-padded)
  uint16_t _blocksH = 0;
  int16_t* _coef = nullptr;
  uint16_t _qt[4][64] = {};  // quantizers in zigzag order
  uint8_t _lumaTq = 0;
};

}  // namespace freeink
