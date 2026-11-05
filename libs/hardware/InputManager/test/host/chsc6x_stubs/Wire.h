#pragma once
// Fake CHSC6x at 0x2E for the Murphy M3 touch test. Shadows picco_stubs/Wire.h; the rest of that
// directory is shared.
#include <cstdint>
#include <vector>
#include "Arduino.h"
struct WireStub {
  // What the controller answers with. A read takes real time on the wire: 16 bytes at the
  // driver's 100 kHz is about 1.7 ms, and the classifiers have to survive that.
  std::vector<uint8_t> frame;
  unsigned long readMs = 2;
  std::vector<uint8_t> rx;
  uint8_t address = 0;
  unsigned offset = 0;
  bool begin(int, int, uint32_t) { return true; }
  void setTimeOut(int) {}
  void beginTransmission(uint8_t a) { address = a; }
  void write(uint8_t) {}
  int endTransmission(bool = true) { return address == 0x2E ? 0 : 2; }
  int requestFrom(uint8_t a, uint8_t n, uint8_t) {
    rx.clear();
    offset = 0;
    if (a != 0x2E) return 0;
    fakeNow += readMs;
    rx = frame;
    rx.resize(n, 0xFF);
    return rx.size();
  }
  int available() { return rx.size() - offset; }
  uint8_t read() { return rx.at(offset++); }
};
inline WireStub Wire;
