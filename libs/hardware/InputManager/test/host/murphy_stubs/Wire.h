#pragma once
// Fake CHSC6x at 0x2e for the Murphy M3 touch test: serves one scripted 16-byte
// frame and records the stop flag of every endTransmission.
#include <cstdint>
#include <cstring>
#include <vector>

#include "Arduino.h"
struct WireStub {
  uint8_t frame[16] = {};  // returned by every read; the test rewrites it
  std::vector<bool> stops;  // stop argument of each endTransmission to 0x2e
  uint8_t address = 0, rx[16] = {};
  unsigned offset = 0, len = 0;
  bool begin(int, int, uint32_t) { return true; }
  void setTimeOut(int) {}
  void beginTransmission(uint8_t a) { address = a; }
  void write(uint8_t) {}
  int endTransmission(bool stop = true) {
    if (address != 0x2e) return 2;
    stops.push_back(stop);
    return 0;
  }
  int requestFrom(uint8_t a, uint8_t n, uint8_t) {
    offset = len = 0;
    if (a != 0x2e || n > sizeof rx) return 0;
    memcpy(rx, frame, n);
    return len = n;
  }
  int available() { return len - offset; }
  uint8_t read() { return rx[offset++]; }
};
inline WireStub Wire;
