#pragma once
#include <cstdint>
struct WireStub {
  void begin(int,int,uint32_t) {}
  void setTimeOut(int) {}
  void end() {}
  void beginTransmission(uint8_t) {}
  void write(uint8_t) {}
  int endTransmission(bool) { return 1; }
  int requestFrom(uint8_t,uint8_t,uint8_t) { return 0; }
  int available() { return 0; }
  uint8_t read() { return 0; }
};
inline WireStub Wire;
