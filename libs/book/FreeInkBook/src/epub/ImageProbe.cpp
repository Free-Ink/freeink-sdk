// FreeInkBook — PNG/JPEG dimension probe without decoding.

#include "epub/ImageProbe.h"

#include <string.h>

namespace freeink {
namespace book {

namespace {

constexpr uint32_t kMaxJpegHeaderScan = 128 * 1024;  // EXIF blobs can be large

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Sequential reader over a ZipEntryReader with exact-read and skip.
struct EntryStream {
  ZipEntryReader& reader;
  uint32_t consumed = 0;

  bool readExact(uint8_t* dst, uint32_t len) {
    uint32_t got = 0;
    while (got < len) {
      const int32_t n = reader.read(dst + got, len - got);
      if (n <= 0) return false;
      got += static_cast<uint32_t>(n);
    }
    consumed += len;
    return true;
  }

  bool skip(uint32_t len, uint8_t* buf, uint32_t bufLen) {
    while (len > 0) {
      const uint32_t want = len < bufLen ? len : bufLen;
      const int32_t n = reader.read(buf, want);
      if (n <= 0) return false;
      len -= static_cast<uint32_t>(n);
      consumed += static_cast<uint32_t>(n);
    }
    return true;
  }
};

}  // namespace

BookStatus probeImage(BookSource& source, const ZipEntry& entry, Arena& scratch,
                      ImageInfo* out) {
  *out = ImageInfo{};
  const size_t marked = scratch.mark();
  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  uint8_t* skipBuf = static_cast<uint8_t*>(scratch.alloc(1024, 1));
  if (status != BookStatus::Ok || skipBuf == nullptr) {
    scratch.release(marked);
    return status != BookStatus::Ok ? status : BookStatus::OutOfMemory;
  }

  EntryStream stream{reader};
  uint8_t hdr[26];
  status = BookStatus::Ok;
  if (!stream.readExact(hdr, 26)) {
    scratch.release(marked);
    return BookStatus::Ok;  // too short to be an image we handle — skip
  }

  static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (memcmp(hdr, kPngSig, 8) == 0 && memcmp(hdr + 12, "IHDR", 4) == 0) {
    const uint32_t w = be32(hdr + 16);
    const uint32_t h = be32(hdr + 20);
    if (w > 0 && h > 0 && w <= 0xFFFF && h <= 0xFFFF) {
      out->kind = ImageInfo::Kind::Png;
      out->width = static_cast<uint16_t>(w);
      out->height = static_cast<uint16_t>(h);
    }
    scratch.release(marked);
    return BookStatus::Ok;
  }

  if (hdr[0] == 0xFF && hdr[1] == 0xD8) {  // JPEG SOI — walk segments to a SOF
    // The first 24 bytes after SOI are already in hdr; rewind logically by
    // treating them as the start of the segment stream.
    uint8_t seg[8];
    uint32_t pending = 24;  // unprocessed bytes sitting in hdr+2
    uint8_t* cur = hdr + 2;
    auto next = [&](uint8_t* dst, uint32_t len) -> bool {
      while (len > 0) {
        if (pending > 0) {
          const uint32_t take = pending < len ? pending : len;
          memcpy(dst, cur, take);
          cur += take;
          pending -= take;
          dst += take;
          len -= take;
        } else {
          return stream.readExact(dst, len);
        }
      }
      return true;
    };
    auto skipN = [&](uint32_t len) -> bool {
      const uint32_t fromPending = pending < len ? pending : len;
      cur += fromPending;
      pending -= fromPending;
      return stream.skip(len - fromPending, skipBuf, 1024);
    };

    while (stream.consumed < kMaxJpegHeaderScan) {
      if (!next(seg, 2)) break;
      if (seg[0] != 0xFF) break;
      const uint8_t marker = seg[1];
      if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
      if (marker == 0xD9 || marker == 0xDA) break;  // EOI / start of scan
      if (!next(seg, 2)) break;
      const uint16_t segLen = be16(seg);
      if (segLen < 2) break;
      const bool isSof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 &&
                         marker != 0xC8 && marker != 0xCC;
      if (isSof) {
        uint8_t sof[5];
        if (next(sof, 5)) {
          out->kind = ImageInfo::Kind::Jpeg;
          out->height = be16(sof + 1);
          out->width = be16(sof + 3);
          out->progressive = marker == 0xC2;
        }
        break;
      }
      if (!skipN(static_cast<uint32_t>(segLen) - 2)) break;
    }
  }

  scratch.release(marked);
  return BookStatus::Ok;
}

}  // namespace book
}  // namespace freeink
