#include "ProgressiveJpeg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define FREEINK_PJ_HEAP_CAPS 1
#endif

namespace freeink {

namespace {

// Zigzag position -> natural (row-major) index in an 8x8 block.
constexpr uint8_t kNatural[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Store limit: keeps the size arithmetic far from overflow on 32-bit targets.
constexpr uint64_t kMaxStoreBytes = 64ull * 1024 * 1024;

void* allocStore(size_t bytes) {
#if FREEINK_PJ_HEAP_CAPS
  return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return calloc(1, bytes);
#endif
}

void freeStore(void* p) {
#if FREEINK_PJ_HEAP_CAPS
  heap_caps_free(p);
#else
  free(p);
#endif
}

// Separable 8x8 inverse DCT: kIdct[x][u] = C(u)/2 * cos((2x+1)u*pi/16).
const float* idctTable() {
  static float t[64];
  static bool ready = false;
  if (!ready) {
    for (int x = 0; x < 8; ++x)
      for (int u = 0; u < 8; ++u)
        t[x * 8 + u] = static_cast<float>((u == 0 ? 0.5 / sqrt(2.0) : 0.5) * cos((2 * x + 1) * u * 3.14159265358979323846 / 16));
    ready = true;
  }
  return t;
}

void idctBlock(const int16_t* coef, const uint16_t* qt, uint8_t* out, uint32_t stride) {
  const float* t = idctTable();
  float f[64], tmp[64];
  for (int k = 0; k < 64; ++k) f[kNatural[k]] = static_cast<float>(coef[k]) * qt[k];
  for (int v = 0; v < 8; ++v)  // rows: horizontal transform
    for (int x = 0; x < 8; ++x) {
      float s = 0;
      for (int u = 0; u < 8; ++u) s += f[v * 8 + u] * t[x * 8 + u];
      tmp[v * 8 + x] = s;
    }
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      float s = 128.5f;
      for (int v = 0; v < 8; ++v) s += tmp[v * 8 + x] * t[y * 8 + v];
      out[y * stride + x] = s <= 0 ? 0 : s >= 255 ? 255 : static_cast<uint8_t>(s);
    }
}

}  // namespace

struct ProgressiveJpeg::Huff {
  int32_t maxCode[18];
  int32_t valPtr[17];
  int32_t minCode[17];
  uint8_t symbols[256];
  bool ready = false;

  void build(const uint8_t counts[16]) {
    int32_t code = 0, index = 0;
    for (int len = 1; len <= 16; ++len) {
      const uint8_t n = counts[len - 1];
      valPtr[len] = index;
      minCode[len] = code;
      code += n;
      index += n;
      maxCode[len] = n ? code - 1 : -1;
      code <<= 1;
    }
    maxCode[17] = 0x7FFFFFFF;
    ready = true;
  }
};

struct ProgressiveJpeg::Comp {
  uint8_t id = 0, h = 1, v = 1, tq = 0;
  uint16_t blocksW = 0, blocksH = 0;  // blocks a non-interleaved scan of this component covers
};

struct ProgressiveJpeg::Reader {
  Reader(ReadFn fn, void* c) : read(fn), ctx(c) {}
  ReadFn read;
  void* ctx;
  uint8_t buf[512];
  uint32_t len = 0, pos = 0;
  bool eof = false, ioError = false;
  uint32_t bits = 0;
  int bitCount = 0;
  int marker = 0;  // marker met inside entropy data; bits read as 0 after it

  int raw() {
    if (pos >= len) {
      if (eof) return -1;
      const int32_t n = read(ctx, buf, sizeof(buf));
      if (n <= 0) {
        eof = true;
        ioError = n < 0;
        return -1;
      }
      len = static_cast<uint32_t>(n);
      pos = 0;
    }
    return buf[pos++];
  }

  bool bytes(uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      const int b = raw();
      if (b < 0) return false;
      dst[i] = static_cast<uint8_t>(b);
    }
    return true;
  }

  bool skip(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
      if (raw() < 0) return false;
    return true;
  }

  void startEntropy() {
    bits = 0;
    bitCount = 0;
    marker = 0;
  }

  int bit() {
    if (bitCount == 0) {
      int b = 0;
      if (!marker) {
        b = raw();
        if (b < 0) {
          b = 0;
        } else if (b == 0xFF) {
          int m = raw();
          while (m == 0xFF) m = raw();
          if (m > 0) {
            marker = m;
            b = 0;
          } else if (m < 0) {
            b = 0;
          }
        }
      }
      bits = static_cast<uint32_t>(b);
      bitCount = 8;
    }
    --bitCount;
    return static_cast<int>((bits >> bitCount) & 1u);
  }

  int32_t get(int n) {
    int32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | bit();
    return v;
  }

  static int32_t extend(int32_t v, int s) { return (s > 0 && v < (1 << (s - 1))) ? v - (1 << s) + 1 : v; }

  int huff(const Huff& h) {
    int32_t code = 0;
    for (int len = 1; len <= 16; ++len) {
      code = (code << 1) | bit();
      if (code <= h.maxCode[len]) {
        const int32_t i = h.valPtr[len] + code - h.minCode[len];
        return i >= 0 && i < 256 ? h.symbols[i] : -1;
      }
    }
    return -1;
  }

  // Discards entropy data up to the next marker other than RSTn; 0 at end of data.
  int nextMarker() {
    bitCount = 0;
    if (marker && (marker < 0xD0 || marker > 0xD7)) {
      const int m = marker;
      marker = 0;
      return m;
    }
    marker = 0;
    for (;;) {
      int b = raw();
      if (b < 0) return 0;
      if (b != 0xFF) continue;
      do b = raw();
      while (b == 0xFF);
      if (b < 0) return 0;
      if (b != 0x00 && (b < 0xD0 || b > 0xD7)) return b;
    }
  }

  // Restart: byte-align and consume RSTn. False when a different marker ends the scan.
  bool restart() {
    bitCount = 0;
    if (marker >= 0xD0 && marker <= 0xD7) {
      marker = 0;
      return true;
    }
    if (marker) return false;
    for (;;) {
      int b = raw();
      if (b < 0) return false;
      if (b != 0xFF) continue;
      do b = raw();
      while (b == 0xFF);
      if (b < 0) return false;
      if (b >= 0xD0 && b <= 0xD7) return true;
      if (b != 0x00) {
        marker = b;
        return false;
      }
    }
  }
};

ProgressiveJpeg::~ProgressiveJpeg() { release(); }

void ProgressiveJpeg::release() {
  if (_coef) freeStore(_coef);
  _coef = nullptr;
}

size_t ProgressiveJpeg::storeBytes(uint16_t w, uint16_t h, uint8_t hmax, uint8_t vmax) {
  const uint64_t bw = (static_cast<uint64_t>(w) + 8 * hmax - 1) / (8 * hmax) * hmax;
  const uint64_t bh = (static_cast<uint64_t>(h) + 8 * vmax - 1) / (8 * vmax) * vmax;
  const uint64_t bytes = bw * bh * 64 * sizeof(int16_t);
  return bytes > kMaxStoreBytes ? 0 : static_cast<size_t>(bytes);
}

ProgressiveJpeg::Status ProgressiveJpeg::begin(ReadFn read, void* ctx) {
  release();
  _width = _height = 0;
  Reader r(read, ctx);
  return parse(r, true);
}

ProgressiveJpeg::Status ProgressiveJpeg::decode(ReadFn read, void* ctx, RowFn rowFn, void* user) {
  if (!_coef) return Status::Unsupported;
  memset(_coef, 0, static_cast<size_t>(_blocksW) * _blocksH * 64 * sizeof(int16_t));
  Reader r(read, ctx);
  const Status status = parse(r, false);
  return status == Status::Ok ? emit(rowFn, user) : status;
}

ProgressiveJpeg::Status ProgressiveJpeg::parse(Reader& r, bool headerOnly) {
  uint8_t soi[2];
  if (!r.bytes(soi, 2) || soi[0] != 0xFF || soi[1] != 0xD8) return r.ioError ? Status::IoError : Status::ParseError;

  Huff dc[4], ac[4];
  Comp comps[4];
  uint8_t ncomp = 0, hmax = 1, vmax = 1;
  uint16_t mcusX = 0, mcusY = 0, restartInterval = 0;
  bool frame = false;
  int pending = 0;

  for (;;) {
    int m = pending;
    pending = 0;
    if (!m) {
      int b = r.raw();
      while (b >= 0 && b != 0xFF) b = r.raw();
      while (b == 0xFF) b = r.raw();
      if (b < 0) break;  // truncated after the last scan: decode what arrived
      m = b;
    }
    if (m == 0xD9) break;  // EOI
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
    uint8_t lb[2];
    if (!r.bytes(lb, 2)) return Status::ParseError;
    uint32_t len = static_cast<uint32_t>(lb[0]) << 8 | lb[1];
    if (len < 2) return Status::ParseError;
    len -= 2;

    if (m == 0xC4) {  // DHT
      while (len > 0) {
        uint8_t tcth, counts[16];
        if (len < 17 || !r.bytes(&tcth, 1) || !r.bytes(counts, 16)) return Status::ParseError;
        uint32_t total = 0;
        for (uint8_t c : counts) total += c;
        const uint8_t th = tcth & 0x0F;
        if (total > 256 || len < 17 + total || th > 3) return Status::ParseError;
        Huff& t = (tcth >> 4) ? ac[th] : dc[th];
        if (!r.bytes(t.symbols, total)) return Status::ParseError;
        t.build(counts);
        len -= 17 + total;
      }
    } else if (m == 0xDB) {  // DQT
      while (len > 0) {
        uint8_t pqtq;
        if (!r.bytes(&pqtq, 1)) return Status::ParseError;
        const uint8_t pq = pqtq >> 4, tq = pqtq & 0x0F;
        const uint32_t n = pq ? 128 : 64;
        if (tq > 3 || len < 1 + n) return Status::ParseError;
        for (int k = 0; k < 64; ++k) {
          uint8_t q[2];
          if (!r.bytes(q, pq ? 2 : 1)) return Status::ParseError;
          _qt[tq][k] = pq ? static_cast<uint16_t>(q[0] << 8 | q[1]) : q[0];
        }
        len -= 1 + n;
      }
    } else if (m == 0xDD) {  // DRI
      uint8_t d[2];
      if (len != 2 || !r.bytes(d, 2)) return Status::ParseError;
      restartInterval = static_cast<uint16_t>(d[0] << 8 | d[1]);
    } else if (m == 0xC2) {  // SOF2: progressive, Huffman
      uint8_t sof[6];
      if (len < 6 || !r.bytes(sof, 6) || sof[0] != 8) return Status::Unsupported;
      const uint16_t h = static_cast<uint16_t>(sof[1] << 8 | sof[2]);
      const uint16_t w = static_cast<uint16_t>(sof[3] << 8 | sof[4]);
      ncomp = sof[5];
      if (!w || !h || !ncomp || ncomp > 4 || len != 6u + 3u * ncomp) return Status::ParseError;
      for (uint8_t c = 0; c < ncomp; ++c) {
        uint8_t cc[3];
        if (!r.bytes(cc, 3)) return Status::ParseError;
        comps[c].id = cc[0];
        comps[c].h = cc[1] >> 4;
        comps[c].v = cc[1] & 0x0F;
        comps[c].tq = cc[2] & 0x03;
        if (comps[c].h < 1 || comps[c].h > 4 || comps[c].v < 1 || comps[c].v > 4) return Status::ParseError;
        if (comps[c].h > hmax) hmax = comps[c].h;
        if (comps[c].v > vmax) vmax = comps[c].v;
      }
      // Luma at full resolution is what the store holds.
      if (comps[0].h != hmax || comps[0].v != vmax) return Status::Unsupported;
      for (uint8_t c = 0; c < ncomp; ++c) {
        comps[c].blocksW = static_cast<uint16_t>(((w * comps[c].h + hmax - 1) / hmax + 7) / 8);
        comps[c].blocksH = static_cast<uint16_t>(((h * comps[c].v + vmax - 1) / vmax + 7) / 8);
      }
      mcusX = static_cast<uint16_t>((w + 8 * hmax - 1) / (8 * hmax));
      mcusY = static_cast<uint16_t>((h + 8 * vmax - 1) / (8 * vmax));
      if (headerOnly) {
        const size_t bytes = storeBytes(w, h, hmax, vmax);
        if (!bytes) return Status::OutOfMemory;
        _coef = static_cast<int16_t*>(allocStore(bytes));
        if (!_coef) return Status::OutOfMemory;
        _width = w;
        _height = h;
        _blocksW = static_cast<uint16_t>(mcusX * hmax);
        _blocksH = static_cast<uint16_t>(mcusY * vmax);
        return Status::Ok;
      }
      if (w != _width || h != _height || mcusX * hmax != _blocksW || mcusY * vmax != _blocksH)
        return Status::ParseError;  // not the stream begin() measured
      _lumaTq = comps[0].tq;
      frame = true;
    } else if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      return Status::Unsupported;  // baseline, lossless, hierarchical or arithmetic
    } else if (m == 0xDA) {  // SOS
      if (!frame) return Status::ParseError;
      uint8_t ns;
      if (!r.bytes(&ns, 1) || ns < 1 || ns > ncomp || len != 1u + 2u * ns + 3u) return Status::ParseError;
      uint8_t idx[4], td[4], ta[4];
      for (uint8_t i = 0; i < ns; ++i) {
        uint8_t sc[2];
        if (!r.bytes(sc, 2)) return Status::ParseError;
        idx[i] = 0xFF;
        for (uint8_t c = 0; c < ncomp; ++c)
          if (comps[c].id == sc[0]) idx[i] = c;
        if (idx[i] == 0xFF) return Status::ParseError;
        td[i] = sc[1] >> 4 & 3;
        ta[i] = sc[1] & 3;
      }
      uint8_t p[3];
      if (!r.bytes(p, 3)) return Status::ParseError;
      const uint8_t ss = p[0], se = p[1], ah = p[2] >> 4, al = p[2] & 0x0F;
      if (ss > se || se > 63 || (ss == 0 && se != 0) || (ss > 0 && ns != 1)) return Status::ParseError;
      r.startEntropy();
      if (idx[0] == 0 || ns > 1) scan(r, comps, ns, idx, ss ? ac[ta[0]] : dc[td[0]], dc, td, ss, se, ah, al,
                                      restartInterval, mcusX, mcusY);
      pending = r.nextMarker();
      if (!pending) break;
    } else if (!r.skip(len)) {
      return Status::ParseError;
    }
  }
  return frame ? Status::Ok : Status::ParseError;
}

void ProgressiveJpeg::scan(Reader& r, const Comp* comps, uint8_t ns, const uint8_t* idx, const Huff& acTable,
                           const Huff* dc, const uint8_t* td, uint8_t ss, uint8_t se, uint8_t ah, uint8_t al,
                           uint16_t restartInterval, uint16_t mcusX, uint16_t mcusY) {
  const bool interleaved = ns > 1;
  const uint32_t unitsW = interleaved ? mcusX : comps[idx[0]].blocksW;
  const uint32_t unitsH = interleaved ? mcusY : comps[idx[0]].blocksH;
  const bool lumaAc = ss > 0;  // AC scans are single-component; only luma's get here
  if (lumaAc && !acTable.ready) return;
  for (uint8_t i = 0; i < ns; ++i)
    if (!lumaAc && ah == 0 && !dc[td[i]].ready) return;

  int32_t pred[4] = {0, 0, 0, 0};
  uint32_t eobrun = 0, sinceRestart = 0;
  const int32_t p1 = 1 << al;

  for (uint32_t uy = 0; uy < unitsH; ++uy) {
    for (uint32_t ux = 0; ux < unitsW; ++ux) {
      if (restartInterval && sinceRestart == restartInterval) {
        if (!r.restart()) return;
        pred[0] = pred[1] = pred[2] = pred[3] = 0;
        eobrun = 0;
        sinceRestart = 0;
      }
      ++sinceRestart;
      for (uint8_t i = 0; i < ns; ++i) {
        const Comp& c = comps[idx[i]];
        const uint8_t bh = interleaved ? c.h : 1, bv = interleaved ? c.v : 1;
        for (uint8_t by = 0; by < bv; ++by) {
          for (uint8_t bx = 0; bx < bh; ++bx) {
            int16_t scratch[64];
            scratch[0] = 0;
            int16_t* blk = scratch;
            if (idx[i] == 0) {
              const uint32_t col = interleaved ? ux * c.h + bx : ux;
              const uint32_t row = interleaved ? uy * c.v + by : uy;
              blk = _coef + (static_cast<size_t>(row) * _blocksW + col) * 64;
            }
            if (!lumaAc) {
              if (ah == 0) {
                const int t = r.huff(dc[td[i]]);
                if (t < 0 || t > 15) return;
                pred[i] += Reader::extend(r.get(t), t);
                blk[0] = static_cast<int16_t>(pred[i] * p1);
              } else if (r.bit()) {
                blk[0] = static_cast<int16_t>(blk[0] | p1);
              }
              continue;
            }
            if (ah == 0) {  // AC first pass
              if (eobrun) {
                --eobrun;
                continue;
              }
              for (int k = ss; k <= se;) {
                const int rs = r.huff(acTable);
                if (rs < 0) return;
                const int run = rs >> 4, s = rs & 15;
                if (s == 0) {
                  if (run < 15) {
                    eobrun = (1u << run) - 1;
                    if (run) eobrun += static_cast<uint32_t>(r.get(run));
                    break;
                  }
                  k += 16;
                  continue;
                }
                k += run;
                if (k > 63) return;
                blk[k++] = static_cast<int16_t>(Reader::extend(r.get(s), s) * p1);
              }
              continue;
            }
            // AC refinement: one correction bit per nonzero coefficient in the
            // band, new coefficients placed after `run` zero ones.
            auto refine = [&](int16_t& coef) {
              if (r.bit() && (coef & p1) == 0) coef = static_cast<int16_t>(coef + (coef > 0 ? p1 : -p1));
            };
            int k = ss;
            if (eobrun) {
              for (; k <= se; ++k)
                if (blk[k]) refine(blk[k]);
              --eobrun;
              continue;
            }
            while (k <= se) {
              const int rs = r.huff(acTable);
              if (rs < 0) return;
              int run = rs >> 4;
              const int s = rs & 15;
              int value = 0;
              if (s == 0) {
                if (run < 15) {
                  eobrun = (1u << run) - 1;
                  if (run) eobrun += static_cast<uint32_t>(r.get(run));
                  run = 64;  // refine the rest of the band, place nothing
                }
              } else {
                if (s != 1) return;
                value = r.bit() ? p1 : -p1;
              }
              while (k <= se) {
                int16_t& coef = blk[k++];
                if (coef) {
                  refine(coef);
                } else {
                  if (run == 0) {
                    coef = static_cast<int16_t>(value);
                    break;
                  }
                  --run;
                }
              }
            }
          }
        }
      }
    }
  }
}

ProgressiveJpeg::Status ProgressiveJpeg::emit(RowFn rowFn, void* user) {
  const uint32_t stride = static_cast<uint32_t>(_blocksW) * 8;
  uint8_t* strip = static_cast<uint8_t*>(malloc(stride * 8));
  if (!strip) return Status::OutOfMemory;
  Status status = Status::Ok;
  const uint16_t* qt = _qt[_lumaTq];
  for (uint32_t by = 0; by * 8 < _height && status == Status::Ok; ++by) {
    for (uint32_t bx = 0; bx < _blocksW; ++bx)
      idctBlock(_coef + (static_cast<size_t>(by) * _blocksW + bx) * 64, qt, strip + bx * 8, stride);
    for (uint32_t y = 0; y < 8 && by * 8 + y < _height; ++y) {
      if (!rowFn(user, static_cast<uint16_t>(by * 8 + y), strip + y * stride, _width)) {
        status = Status::Stopped;
        break;
      }
    }
  }
  free(strip);
  return status;
}

}  // namespace freeink
