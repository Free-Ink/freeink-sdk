#pragma once
#include <Arduino.h>
#include <BoardConfig.h>
#include <PaperMonoBoard.h>
#include <Rtc.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
std::array<std::array<uint8_t, 256>, 128> regs{};
struct Write {
  uint8_t addr, reg, value;
};
std::vector<Write> writes;
int operations = 0, failAt = 0;
bool deadBus = false, dropCounter = false, dropEnable = false;
bool fail() { return deadBus || ++operations == failAt; }
void reset() {
  regs = {};
  writes.clear();
  operations = failAt = 0;
  deadBus = dropCounter = dropEnable = false;
  BoardConfig::ACTIVE.sensors.rtcType = BoardConfig::RtcType::Rx8130;
}
}  // namespace

bool hostI2cWrite(uint8_t addr, const std::vector<uint8_t>& bytes) {
  if (fail()) return false;
  if (bytes.empty()) return true;
  for (size_t i = 1; i < bytes.size(); ++i) {
    const uint8_t reg = bytes[0] + i - 1, value = bytes[i];
    writes.push_back({addr, reg, value});
    if (addr == 0x32 && dropCounter && reg == 0x1A) continue;
    if (addr == 0x32 && dropEnable && reg == 0x1C && (value & 0x10)) continue;
    const bool rx8010 = BoardConfig::ACTIVE.sensors.rtcType == BoardConfig::RtcType::Rx8010;
    if (addr == 0x32 && rx8010 && reg == 0x1E) {
      regs[addr][reg] &= value;  // W0C.
    } else if (addr == 0x32 && reg == 0x1D) {
      assert(!rx8010);  // There it is the extension register: only the RX8130's flags live here.
      assert(!(value & 0x40));
      regs[addr][reg] &= value | 0x01;  // W0C; VBFF is read-only.
    } else if (addr == 0x6E && reg == 0x05) {
      regs[addr][reg] &= value;
    } else {
      regs[addr][reg] = value;
    }
  }
  return true;
}
bool hostI2cRead(uint8_t addr, uint8_t reg, uint8_t* out, size_t length) {
  if (fail()) return false;
  for (size_t i = 0; i < length; ++i) out[i] = regs[addr][reg + i];
  if (addr == 0x6E && reg == 0x48) regs[addr][reg] &= ~0x80;
  return true;
}
