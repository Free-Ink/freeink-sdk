#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <cstddef>
struct TwoWire {
  struct Write { uint8_t addr, reg, value; };
  std::array<std::array<uint8_t,256>,128> regs{};
  std::array<bool,128> devices{};
  std::vector<Write> writes;
  std::vector<uint8_t> tx, rx;
  uint8_t address = 0, reg = 0, lastRead = 0;
  size_t offset = 0;
  int failWriteReg = -1, failReadReg = -1;
  bool shortRead = false;
  uint16_t timeoutMs = 4, uploadTimeoutMs = 0;
  // BMI270 model (0x68/0x69): INIT_DATA is a window, not a register run. The
  // file is collected in order of the INIT_ADDR word offset; INIT_CTRL = 1
  // after a complete file reports init_ok unless bmiRefuseInit.
  std::vector<uint8_t> bmiConfig;
  bool bmiBadOffset = false, bmiRefuseInit = false;
  int bmiUploads = 0;
  static bool bmi(uint8_t a) { return a == 0x68 || a == 0x69; }
  TwoWire() {
    devices[0x20] = true;
    regs[0x20][6] = regs[0x20][7] = 255;
  }
  bool begin(int sda, int scl, uint32_t hz) { return sda == 41 && scl == 42 && hz == 400000; }
  bool end() { return true; }
  void setTimeOut(uint16_t ms) { timeoutMs = ms; }
  uint16_t getTimeOut() const { return timeoutMs; }
  void beginTransmission(uint8_t a) { address = a; tx.clear(); }
  void write(uint8_t b) { tx.push_back(b); }
  int endTransmission(bool = true) {
    if (!devices[address]) return 2;
    if (tx.empty()) return 0;
    reg = tx[0];
    if (tx.size() > 1) {
      if (address != 0x20 && reg == failWriteReg) return 2;
      if (bmi(address) && bmiWrite()) return 0;
      for (size_t i = 1; i < tx.size(); ++i) {
        regs[address][reg + i - 1] = tx[i];
        writes.push_back({address, uint8_t(reg + i - 1), tx[i]});
      }
    }
    return 0;
  }
  int requestFrom(uint8_t a, uint8_t n, uint8_t) {
    rx.clear(); offset = 0; lastRead = reg;
    if (!devices[a] || (a != 0x20 && (reg & 0x7F) == failReadReg)) return 0;
    const bool sc7 = a == 0x18 || a == 0x19;
    const bool increment = !sc7 || (reg & 0x80);
    uint8_t start = sc7 ? reg & 0x7F : reg;
    if (shortRead && a != 0x20 && n > 1) --n;
    for (int i = 0; i < n; ++i) rx.push_back(regs[a][start + (increment ? i : 0)]);
    return rx.size();
  }
  uint8_t read() { return rx.at(offset++); }
  bool bmiWrite() {
    auto& r = regs[address];
    if (reg == 0x5E) {
      const size_t at = 2 * ((r[0x5B] & 0x0F) | size_t(r[0x5C]) << 4);
      if (at != bmiConfig.size()) bmiBadOffset = true;
      if (bmiConfig.empty()) ++bmiUploads;
      uploadTimeoutMs = timeoutMs;
      bmiConfig.insert(bmiConfig.end(), tx.begin() + 1, tx.end());
      return true;
    }
    if (reg == 0x7E && tx[1] == 0xB6) {  // soft reset: config file lost
      bmiConfig.clear();
      r[0x21] = 0; r[0x7C] = 3; r[0x7D] = 0;
      return true;
    }
    if (reg == 0x59 && tx[1] == 1 && bmiConfig.size() == 8192 && !bmiRefuseInit) r[0x21] = 1;
    return false;
  }
};
inline TwoWire Wire;
