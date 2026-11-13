#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif

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
    _median = 0;
    _rows[0] = _rows[1] = 0;
    _failed = false;
  }
  void begin(const uint8_t *bw) {
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
  bool compose() {
    if (!_active || _failed || _rows[0] != _h || _rows[1] != _h)
      return false;
    auto *lo = _storage + _size;
    auto *hi = _storage + _size * 2;
    const auto *ink = _storage + _size * 3;
    const auto *charge = reinterpret_cast<const int8_t *>(_storage + _size * 4);
    const int threshold =
        _median + 11; // ceil(10 * 1024 / 998), stock threshold mask.
    for (uint32_t i = 0; i < _size; ++i) {
      const uint8_t gray = lo[i] | hi[i];
      const uint8_t white = _storage[i] & ~gray;
      uint8_t hold = _history ? static_cast<uint8_t>(white & ~ink[i]) : 0;
      if (_chargeValid) {
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
    auto *ink = _storage + _size * 3;
    auto *charge = reinterpret_cast<int8_t *>(_storage + _size * 4);
    const int doses[4] = {blackDose, grayDose, holdDose, whiteDose};
    uint32_t histogram[256] = {};
    for (uint32_t i = 0; i < _size; ++i) {
      const auto lo = low()[i], hi = high()[i];
      ink[i] = ~hi;
      for (unsigned bit = 0; bit < 8; ++bit) {
        const unsigned shift = 7 - bit;
        const unsigned code = ((hi >> shift) & 1) * 2 + ((lo >> shift) & 1);
        int v = 0;
        if (_chargeValid) {
          v = (static_cast<int>(charge[i * 8 + bit]) - _median) * 998;
          v = v >= 0 ? v / 1024 : -((-v + 1023) / 1024);
        }
        v += doses[code];
        v = v < -127 ? -127 : v > 127 ? 127 : v;
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
    _active = false;
    _history = _chargeValid = _onPanel = true;
  }

private:
  uint8_t *_storage = nullptr;
  uint32_t _size = 0;
  uint16_t _wb = 0, _h = 0;
  uint16_t _rows[2] = {};
  int _median = 0;
  bool _enabled = false;
  bool _active = false, _onPanel = false, _history = false,
       _chargeValid = false, _failed = false;
};
} // namespace freeink
