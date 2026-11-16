#pragma once
#include <Arduino.h>

#include <array>
#include <vector>
struct WireStub {
  std::array<uint8_t, 256> regs{};
  std::vector<uint8_t> tx;
  uint8_t address = 0, pointer = 0;
  unsigned long awakeAt = 0;
  unsigned rejectedWrites = 0;
  int failRegister = -1;
  bool present = true;
  WireStub() { regs[0] = 0x80; }
  void begin(int, int, uint32_t) {}
  void setTimeOut(unsigned) {}
  void beginTransmission(uint8_t addr) {
    address = addr;
    tx.clear();
  }
  void write(uint8_t value) { tx.push_back(value); }
  uint8_t control() const { return regs[0] | ((regs[0] & 0x80) || fakeNow < awakeAt ? 0x40 : 0); }
  int endTransmission(bool = true) {
    if (address != 0x38 || !present) return 2;
    if (tx.empty()) return 0;
    pointer = tx[0];
    if (tx.size() == 1) return 0;
    if (pointer == failRegister) {
      failRegister = -1;
      return 2;
    }
    // Model the wake settling interval: the bus can ACK the wake command,
    // but brightness/current/enable writes sent immediately afterward fail.
    if (fakeNow < awakeAt) {
      ++rejectedWrites;
      return 2;
    }
    const uint8_t value = tx[1];
    if (pointer == 0) {
      if ((regs[0] & 0x80) && !(value & 0x80)) awakeAt = fakeNow + 2;
      regs[0] = value & ~0x40;  // SLEEP_STATUS is read-only
    } else
      regs[pointer] = value;
    return 0;
  }
  uint8_t requestFrom(uint8_t addr, uint8_t n, uint8_t) { return present && addr == 0x38 ? n : 0; }
  uint8_t read() { return pointer == 0 ? control() : regs[pointer]; }
  bool visible() const {
    return !(regs[0] & 0x80) && fakeNow >= awakeAt && (((regs[0] & 4) && regs[3]) || ((regs[0] & 2) && regs[4]));
  }
};
inline WireStub Wire;
