#pragma once
// Fake Parade TMA525C (PIP) at 0x24 for the Picco touch test.
#include <cstdint>
#include <deque>
#include <vector>

#include "Arduino.h"
struct WireStub {
  std::deque<std::vector<uint8_t>> reports;  // pending PIP reports (INT low while non-empty)
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint8_t> tx, rx;
  uint8_t address = 0, mode = 0xF7;
  unsigned offset = 0;
  uint8_t maxContacts = 4;
  bool replySysinfo = true;
  uint8_t sysinfoLength = 51;
  uint8_t sysinfoCommand = 2;
  bool begin(int, int, uint32_t) { return true; }
  void setTimeOut(int) {}
  void beginTransmission(uint8_t a) {
    address = a;
    tx.clear();
  }
  void write(uint8_t v) { tx.push_back(v); }
  void write(const uint8_t* v, size_t n) { tx.insert(tx.end(), v, v + n); }
  void sync() { gpioLevels[11] = reports.empty() ? 1 : 0; }
  void push(std::vector<uint8_t> r) {
    reports.push_back(r);
    sync();
  }
  int endTransmission(bool = true) {
    if (address != 0x24) return 2;
    writes.push_back(tx);
    if (tx == std::vector<uint8_t>{0x01, 0x00})
      push({0x20, 0x00, mode, 0x00});  // HID descriptor reply
    else if (tx.size() == 13 && tx[7] == 0x3B) {
      mode = 0xF7;
      push({0x05, 0x00, 0x1F, 0x00, 0x3B});
    } else if (tx == std::vector<uint8_t>{0x04, 0x00, 0x05, 0x00, 0x2F, 0x00, 0x02} && replySysinfo) {
      std::vector<uint8_t> info(51, 0);
      info[0] = sysinfoLength;
      info[2] = 0x1F;
      info[4] = sysinfoCommand;
      info[50] = maxContacts;
      info.resize(sysinfoLength);
      push(info);
    }
    return 0;
  }
  int requestFrom(uint8_t a, uint8_t n, uint8_t) {
    rx.clear();
    offset = 0;
    if (a != 0x24 || reports.empty()) {
      if (n == 2) rx = {0, 0};
      return rx.size();
    }
    const auto& r = reports.front();
    if (n == 2) {
      rx = {r[0], r[1]};
      return 2;
    }
    rx = r;
    rx.resize(n);
    reports.pop_front();
    sync();
    return rx.size();
  }
  int available() { return rx.size() - offset; }
  uint8_t read() { return rx.at(offset++); }
};
inline WireStub Wire;
