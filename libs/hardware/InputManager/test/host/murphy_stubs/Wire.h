#pragma once

#include <cstddef>
#include <cstdint>

inline int fakeWireCalls = 0;

struct TwoWire {
  bool begin(int, int, uint32_t) {
    ++fakeWireCalls;
    return true;
  }
  void setTimeOut(int) { ++fakeWireCalls; }
  void beginTransmission(uint8_t) { ++fakeWireCalls; }
  void write(uint8_t) { ++fakeWireCalls; }
  void write(const uint8_t*, size_t) { ++fakeWireCalls; }
  int endTransmission(bool = true) { ++fakeWireCalls; return 0; }
  int requestFrom(uint8_t, uint8_t, uint8_t) { ++fakeWireCalls; return 0; }
  int available() { ++fakeWireCalls; return 0; }
  uint8_t read() { ++fakeWireCalls; return 0; }
};
inline TwoWire Wire;
inline TwoWire Wire1;