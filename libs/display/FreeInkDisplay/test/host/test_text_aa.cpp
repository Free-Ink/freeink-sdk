// Scalar reference for the charge history: optimizations must preserve every
// transition, charge byte and median, including after cancellation/reset.
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#define private public
#include "X4ProTextAa.h"
#undef private

struct Reference {
  std::vector<uint8_t> base, lo, hi, ink;
  std::vector<int8_t> charge;
  int median = 0;
  bool valid = false;
  explicit Reference(size_t n) : base(n), lo(n), hi(n), ink(n), charge(n * 8) {}
  void compose() {
    for (size_t i = 0; i < base.size(); ++i) {
      const uint8_t gray = lo[i] | hi[i];
      const uint8_t white = base[i] & ~gray;
      uint8_t hold = valid ? static_cast<uint8_t>(white & ~ink[i]) : 0;
      if (valid) {
        for (unsigned bit = 0; bit < 8; ++bit)
          if (charge[i * 8 + bit] > median + 11) hold &= static_cast<uint8_t>(~(0x80 >> bit));
      }
      lo[i] = gray | (white & ~hold);
      hi[i] = white;
    }
  }
  void finish(const std::array<int, 4>& doses) {
    uint32_t histogram[256] = {};
    for (size_t i = 0; i < base.size(); ++i) {
      ink[i] = ~hi[i];
      for (unsigned bit = 0; bit < 8; ++bit) {
        const unsigned shift = 7 - bit;
        const unsigned code = ((hi[i] >> shift) & 1) * 2 + ((lo[i] >> shift) & 1);
        int v = 0;
        if (valid) {
          v = ((static_cast<int>(charge[i * 8 + bit]) - median) * 998 + 512) >> 10;
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
      if (count > base.size() * 4 - 1) {
        median = i - 128;
        break;
      }
    }
    valid = true;
  }
};

static void compare(const freeink::X4ProTextAa& aa, const Reference& ref) {
  const size_t n = ref.base.size();
  assert(memcmp(aa.low(), ref.lo.data(), n) == 0);
  assert(memcmp(aa.high(), ref.hi.data(), n) == 0);
  assert(memcmp(aa._storage + n * 3, ref.ink.data(), n) == 0);
  assert(memcmp(aa._storage + n * 4, ref.charge.data(), n * 8) == 0);
  assert(aa._median == ref.median && aa._chargeValid == ref.valid);
}

static void testHistory() {
  freeink::X4ProTextAa aa;
  aa.allocate(true, 100, 480);
  Reference ref(aa._size);
  std::mt19937 random(12345);
  const std::array<int, 4> doses[] = {{{24, 15, -1, -24}}, {{20, 2, 2, -15}}, {{-127, 127, -1, 0}}};
  for (int page = 0; page < 400; ++page) {
    if (page % 37 == 0) {
      aa.invalidate();
      ref.valid = false;
      ref.median = 0;
    }
    for (size_t i = 0; i < ref.base.size(); ++i) {
      // Mix uniform paper/strokes with arbitrary byte patterns and grays.
      ref.base[i] = i % 4 ? (page % 3 ? 0xff : 0) : random();
      ref.lo[i] = i % 4 ? 0 : random();
      ref.hi[i] = i % 4 ? 0 : random();
    }
    aa.begin(ref.base.data());
    aa.stage(0, ref.lo.data(), 0, 480);
    aa.stage(1, ref.hi.data(), 0, 480);
    assert(aa.compose());
    ref.compose();
    assert(memcmp(aa.low(), ref.lo.data(), ref.lo.size()) == 0);
    assert(memcmp(aa.high(), ref.hi.data(), ref.hi.size()) == 0);
    const auto& d = doses[page % 3];
    aa.finish(d[0], d[1], d[2], d[3]);
    ref.finish(d);
    compare(aa, ref);
  }
  // Cover every prior charge and transition code in each full-size frame.
  // Repeat for every valid median, keeping the fixed-size production API.
  const size_t n = ref.base.size();
  for (int median = -127; median <= 127; ++median) {
    for (size_t i = 0; i < n; ++i) {
      const unsigned code = i % 4;
      const int previous = static_cast<int>((i / 4) % 255) - 127;
      aa._storage[n + i] = ref.lo[i] = code & 1 ? 0xff : 0;
      aa._storage[n * 2 + i] = ref.hi[i] = code & 2 ? 0xff : 0;
      memset(aa._storage + n * 4 + i * 8, static_cast<uint8_t>(previous), 8);
      memset(ref.charge.data() + i * 8, static_cast<uint8_t>(previous), 8);
    }
    aa._chargeValid = ref.valid = true;
    aa._median = ref.median = median;
    aa.finish(24, 15, -1, -24);
    ref.finish(doses[0]);
    compare(aa, ref);
  }
}

static void testWindowTransitions() {
  freeink::X4ProTextAa aa;
  aa.allocate(true, 1, 1);
  const uint8_t base = 0x58;
  const uint8_t gray = 0x31;
  const uint8_t empty = 0;
  aa.begin(&base);
  aa.stage(0, &gray, 0, 1);
  aa.stage(1, &empty, 0, 1);
  assert(aa.compose());
  aa.finish(24, 15, -1, -24);

  // Keep black at x=0 and white at x=1. Replace gray with black at x=2
  // and with white at x=3. Keep the gray at x=7 outside the window.
  uint8_t grayMask = 0;
  aa.writeGrayMask(&grayMask);
  const uint8_t update = 0x58;
  for (uint8_t expectedHigh : {0xdf, 0xff}) {
    assert(aa.beginWindow(&update, 0, 0, 4, 1));
    assert(aa.high()[0] == expectedHigh);
    assert(aa.low()[0] == (expectedHigh == 0xdf ? 0x10 : 0));
    aa.finish(24, 15, -1, -24);
    aa.writeGrayMask(&grayMask);
    assert(grayMask == 0x01);
  }

  // Later windows still recognize the untouched gray, even over a black base.
  assert(aa.beginWindow(&update, 7, 0, 1, 1));
  assert(aa.high()[0] == 0xfe && aa.low()[0] == 0);
  aa.finish(24, 15, -1, -24);
  aa.writeGrayMask(&grayMask);
  assert(grayMask == 0);
}

int main() {
  testHistory();
  testWindowTransitions();
  puts("Text AA: scalar parity across 400 pages and all charge/median/code combinations passed");
}
