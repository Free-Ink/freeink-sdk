#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif

// X4 Pro and Classic UltraChip text pages: every Nth one-pass page drives all
// white pixels; 0 disables it. The count restarts after any B/W refresh.
#ifndef FREEINK_X4PRO_TEXT_REDRIVE_PAGES
#define FREEINK_X4PRO_TEXT_REDRIVE_PAGES 4
#endif
static_assert(FREEINK_X4PRO_TEXT_REDRIVE_PAGES >= 0 && FREEINK_X4PRO_TEXT_REDRIVE_PAGES <= 255,
              "FREEINK_X4PRO_TEXT_REDRIVE_PAGES must fit in uint8_t");

namespace freeink {

// Full-target text path: black, one AA gray, and charge-aware white.
class X4ProTextAa {
public:
  ~X4ProTextAa() { free(_storage); }
  X4ProTextAa() = default;
  X4ProTextAa(const X4ProTextAa &) = delete;
  X4ProTextAa &operator=(const X4ProTextAa &) = delete;

  // Sized to the panel: one 1bpp plane is widthBytes x height bytes; the
  // helper holds 12 of them (three planes, an ink plane, a byte per pixel).
  void allocate(bool enabled, uint16_t widthBytes, uint16_t height) {
    const uint32_t size = static_cast<uint32_t>(widthBytes) * height;
    if (size != _size) {
      free(_storage);
      _storage = nullptr;
    }
    _wb = widthBytes;
    _h = height;
    _size = size;
    _enabled = enabled && size != 0;
#if defined(BOARD_HAS_PSRAM)
    if (_enabled && !_storage)
      _storage = static_cast<uint8_t *>(
          heap_caps_malloc(size * 12, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
    invalidate();
  }
  bool ready() const { return _enabled && _storage != nullptr; }
  bool active() const { return _active; }
  bool onPanel() const { return _onPanel; }
  const uint8_t *base() const { return _storage; }
  const uint8_t *low() const { return _storage + _size; }
  const uint8_t *high() const { return _storage + _size * 2; }
  void invalidate() {
    _active = _onPanel = _history = _chargeValid = false;
    _window = {};
    _heldPages = 0;
    _median = 0;
    _rows[0] = _rows[1] = 0;
    _failed = false;
  }
  void begin(const uint8_t *bw) {
    _window = {};
    _active = ready() && bw;
    _rows[0] = _rows[1] = 0;
    _failed = false;
    if (_active)
      memcpy(_storage, bw, _size);
  }
  void stage(unsigned plane, const uint8_t *data, uint16_t y, uint16_t rows) {
    if (!ready() || plane > 1)
      return;
    if (!_active && plane == 0 && y == 0) {
      _rows[0] = _rows[1] = 0;
      _failed = false;
    }
    if (!data || !rows || y != _rows[plane] || y >= _h || rows > _h - y) {
      _failed = true;
      return;
    }
    memcpy(_storage + _size * (plane + 1) + static_cast<uint32_t>(y) * _wb, data,
           static_cast<uint32_t>(rows) * _wb);
    _rows[plane] += rows;
  }
  bool planeComplete(unsigned plane) const {
    return !_failed && _rows[plane] == _h;
  }
  bool canUpdateWindow() const { return _onPanel && !_active; }

  bool beginWindow(const uint8_t* bw, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!canUpdateWindow() || !bw) return false;
    const uint16_t lastPixel = x + w - 1;
    _window = {y,
               static_cast<uint16_t>(y + h),
               static_cast<uint16_t>(x / 8),
               static_cast<uint16_t>(lastPixel / 8),
               static_cast<uint8_t>(0xff >> (x % 8)),
               static_cast<uint8_t>(0xff << (7 - lastPixel % 8))};
    _active = true;
    auto* lo = _storage + _size;
    auto* hi = _storage + _size * 2;
    auto* ink = _storage + _size * 3;
    // High/low = 10 holds pixels that are outside the window or already match.
    memset(lo, 0, _size);
    memset(hi, 0xff, _size);
    forEachWindowByte([&](uint32_t i, uint8_t mask) {
      const uint8_t white = bw[i] & mask;
      const uint8_t toBlack = mask & ~white & _storage[i];
      lo[i] = white & ink[i];
      hi[i] = ~toBlack;
      _storage[i] = (_storage[i] & ~mask) | white;
      ink[i] = (ink[i] & ~mask) | (~white & mask);
    });
    return true;
  }

  void writeGrayMask(uint8_t* output) const {
    if (_window.endRow != 0) {
      // A window contains only B/W; the previous gray mask elsewhere stays valid.
      forEachWindowByte([&](uint32_t i, uint8_t mask) { output[i] &= ~mask; });
      return;
    }
    for (uint32_t i = 0; i < _size; ++i) output[i] = low()[i] & ~high()[i];
  }

  // Held white pixels get only a short pulse, so residue the model cannot see
  // (from menus, covers or a weak clean) stays under them. With a nonzero
  // redrivePeriod, every redrivePeriod-th page drives all white pixels.
  bool compose(uint8_t redrivePeriod = 0) {
    if (_window.endRow != 0) return _active;
    if (!_active || _failed || _rows[0] != _h || _rows[1] != _h)
      return false;
    auto *lo = _storage + _size;
    auto *hi = _storage + _size * 2;
    const auto *ink = _storage + _size * 3;
    const auto *charge = reinterpret_cast<const int8_t *>(_storage + _size * 4);
    const int threshold =
        _median + 11; // ceil(10 * 1024 / 998), stock threshold mask.
    _redrivePage = !_history || (redrivePeriod != 0 && _heldPages + 1u >= redrivePeriod);
    for (uint32_t i = 0; i < _size; ++i) {
      const uint8_t gray = lo[i] | hi[i];
      const uint8_t white = _storage[i] & ~gray;
      uint8_t hold = _redrivePage ? 0 : static_cast<uint8_t>(white & ~ink[i]);
      if (_chargeValid && hold) {
        for (unsigned bit = 0; bit < 8; ++bit)
          if (charge[i * 8 + bit] > threshold)
            hold &= static_cast<uint8_t>(~(0x80 >> bit));
      }
      lo[i] = gray | (white & ~hold);
      hi[i] = white;
    }
    return true;
  }
  void finish(int blackDose, int grayDose, int holdDose, int whiteDose) {
    auto* ink = _storage + _size * 3;
    auto* whitePlane = _storage + _size * 2;
    auto* charge = reinterpret_cast<int8_t*>(_storage + _size * 4);
    const int doses[4] = {blackDose, grayDose, holdDose, whiteDose};
    uint32_t histogram[256] = {};
    // -Os otherwise outlines this lambda on Xtensa, adding a call per pixel.
    const auto nextCharge = [&](int previous, unsigned code) __attribute__((always_inline)) {
      int v = 0;
      if (_chargeValid) {
        // Rounds to nearest, as firmware 26110201 does (0x421877f6: +512, >>10).
        v = ((previous - _median) * 998 + 512) >> 10;
      }
      v += doses[code];
      return v < -127 ? -127 : v > 127 ? 127 : v;
    };
    for (uint32_t i = 0; i < _size; ++i) {
      const auto lo = low()[i], hi = high()[i];
      if (_window.endRow == 0) {
        ink[i] = ~hi;
        // The B/W baseline treats gray as white, while ink includes gray.
        // Together they distinguish gray from both solid colors in later windows.
        _storage[i] = hi | lo;
      } else {
        whitePlane[i] = ~ink[i];  // B/W cleanup needs the whole shown baseline.
      }
      // Blank paper and solid strokes commonly have eight equal transitions
      // and charges. Update those pixels together, including their histogram
      // weight. memcpy keeps this safe on unaligned PSRAM addresses.
      if ((lo == 0 || lo == 0xff) && (hi == 0 || hi == 0xff)) {
        constexpr uint64_t repeatByte = UINT64_C(0x0101010101010101);
        uint64_t previous = 0;
        if (_chargeValid) memcpy(&previous, charge + i * 8, sizeof(previous));
        const uint8_t first = static_cast<uint8_t>(previous);
        if (!_chargeValid || previous == repeatByte * first) {
          const unsigned code = (hi & 2) | (lo & 1);
          const int v = nextCharge(static_cast<int8_t>(first), code);
          const uint64_t repeated = repeatByte * static_cast<uint8_t>(v);
          memcpy(charge + i * 8, &repeated, sizeof(repeated));
          histogram[v + 128] += 8;
          continue;
        }
      }
      for (unsigned bit = 0; bit < 8; ++bit) {
        const unsigned shift = 7 - bit;
        const unsigned code = ((hi >> shift) & 1) * 2 + ((lo >> shift) & 1);
        const int v = nextCharge(_chargeValid ? charge[i * 8 + bit] : 0, code);
        charge[i * 8 + bit] = static_cast<int8_t>(v);
        ++histogram[v + 128];
      }
    }
    uint32_t count = 0;
    for (int i = 0; i < 256; ++i) {
      count += histogram[i];
      if (count > _size * 4 - 1) {
        _median = i - 128;
        break;
      }
    }
    if (_window.endRow == 0) _heldPages = _redrivePage ? 0 : static_cast<uint8_t>(_heldPages + 1);
    _active = false;
    _history = _chargeValid = _onPanel = true;
  }

private:
 struct Window {
   uint16_t firstRow = 0, endRow = 0;
   uint16_t firstByte = 0, lastByte = 0;
   uint8_t headMask = 0, tailMask = 0;
 } _window;

 template <typename Visit>
 void forEachWindowByte(Visit visit) const {
   for (uint16_t row = _window.firstRow; row < _window.endRow; ++row) {
     for (uint16_t byte = _window.firstByte; byte <= _window.lastByte; ++byte) {
       uint8_t mask = 0xff;
       if (byte == _window.firstByte) mask &= _window.headMask;
       if (byte == _window.lastByte) mask &= _window.tailMask;
       visit(static_cast<uint32_t>(row) * _wb + byte, mask);
     }
   }
 }

  uint8_t *_storage = nullptr;
  uint32_t _size = 0;
  uint16_t _wb = 0, _h = 0;
  uint16_t _rows[2] = {};
  int _median = 0;
  // Whole-screen text pages since every white pixel was last driven.
  uint8_t _heldPages = 0;
  bool _redrivePage = false;
  bool _enabled = false;
  bool _active = false, _onPanel = false, _history = false,
       _chargeValid = false, _failed = false;
};
} // namespace freeink
