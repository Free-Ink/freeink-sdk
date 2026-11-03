// Paper Mono bring-up (PaperMonoBoard.h) against a model PMIC and IO expander.
// The bring-up keeps its state in function statics, so each scenario is its own run.
#include <Arduino.h>
#include <PaperMonoBoard.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace ioe1 = freeink::m5ioe1;

constexpr uint8_t PM1 = 0x6E;
std::array<std::array<uint8_t, 256>, 128> regs{};
unsigned long expanderUpAt = 0;  // the expander NACKs until hostMillis reaches this
int failExpanderReadAfterRail = 0;  // fail the Nth expander read after TP_EN first goes high
bool dropTouchReset = false;        // the expander ignores TP_RST writes
bool railSeen = false;
int readsAfterRail = 0;
int touchResetReleases = 0;
int busOperations = 0;

bool isExpander(uint8_t addr) { return addr == ioe1::ADDR_PRIMARY || addr == ioe1::ADDR_ALT; }

uint16_t expanderOut() {
  const auto& r = regs[ioe1::ADDR_PRIMARY];
  return static_cast<uint16_t>(r[ioe1::REG_GPIO_OUT_L] | (r[ioe1::REG_GPIO_OUT_L + 1] << 8));
}
}  // namespace

bool hostI2cWrite(uint8_t addr, const std::vector<uint8_t>& bytes) {
  ++busOperations;
  if (isExpander(addr) && hostMillis < expanderUpAt) return false;
  if (bytes.empty()) return true;
  const uint16_t before = expanderOut();
  for (size_t i = 1; i < bytes.size(); ++i) regs[addr][bytes[0] + i - 1] = bytes[i];
  if (addr == ioe1::ADDR_PRIMARY && bytes[0] == ioe1::REG_GPIO_OUT_L) {
    uint16_t out = expanderOut();
    if (dropTouchReset) {
      out = static_cast<uint16_t>((out & ~ioe1::PIN_TOUCH_RESET) | (before & ioe1::PIN_TOUCH_RESET));
      regs[addr][ioe1::REG_GPIO_OUT_L] = out & 0xFF;
      regs[addr][ioe1::REG_GPIO_OUT_L + 1] = out >> 8;
    }
    if (out & ioe1::PIN_TOUCH_POWER) railSeen = true;
    if ((out & ioe1::PIN_TOUCH_RESET) && !(before & ioe1::PIN_TOUCH_RESET)) ++touchResetReleases;
  }
  return true;
}

bool hostI2cRead(uint8_t addr, uint8_t reg, uint8_t* out, size_t length) {
  ++busOperations;
  if (isExpander(addr) && hostMillis < expanderUpAt) return false;
  if (isExpander(addr) && railSeen && ++readsAfterRail == failExpanderReadAfterRail) return false;
  for (size_t i = 0; i < length; ++i) out[i] = regs[addr][reg + i];
  return true;
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const char* scenario = argv[1];
  constexpr uint16_t touchUp = ioe1::PIN_TOUCH_POWER | ioe1::PIN_TOUCH_RESET;
  regs[PM1][0x05] = 0x20;  // latched wake source

  if (!strcmp(scenario, "healthy")) {
    assert(freeink::papermono::ensureBooted(2000));
    assert(hostMillis == 0);
    assert(freeink::papermono::bootWakeState().valid && freeink::papermono::bootWakeState().source == 0x20);
    assert(regs[PM1][0x05] == 0);
    assert(freeink::papermono::enableTouch());
    assert((expanderOut() & touchUp) == touchUp && touchResetReleases == 1);
  } else if (!strcmp(scenario, "late-expander")) {
    // Seen after a PMIC restart: the expander misses the first probes. One probe used to
    // mark it absent for the session.
    expanderUpAt = 600;
    assert(freeink::papermono::ensureBooted(2000));
    assert(hostMillis >= 600 && hostMillis < 700);
    const uint16_t mode = regs[ioe1::ADDR_PRIMARY][ioe1::REG_GPIO_MODE_L] |
                          (regs[ioe1::ADDR_PRIMARY][ioe1::REG_GPIO_MODE_L + 1] << 8);
    assert((mode & ioe1::OUTPUT_MASK) == ioe1::OUTPUT_MASK);
    assert(freeink::papermono::bootWakeState().source == 0x20);  // snapshot from the first attempt
    assert(freeink::papermono::enableTouch());
  } else if (!strcmp(scenario, "missing-expander")) {
    expanderUpAt = 1000000;
    assert(!freeink::papermono::ensureBooted(2000));
    assert(hostMillis >= 2000 && hostMillis < 2100);
    const int operations = busOperations;
    assert(!freeink::papermono::ensureBooted());  // inside the retry gap: no bus traffic
    assert(busOperations == operations);
    assert(!freeink::papermono::enableTouch());
    delay(500);
    assert(!freeink::papermono::ensureBooted());  // after the gap: probes again
    assert(busOperations > operations);
    expanderUpAt = 0;
    delay(500);
    assert(freeink::papermono::ensureBooted());  // a late expander still comes up
  } else if (!strcmp(scenario, "failed-read-during-touch")) {
    // Seen on the device: OUT 0x1000 (rail on, reset low) after one failed read in the
    // read-modify-write that releases the reset.
    assert(freeink::papermono::ensureBooted(2000));
    failExpanderReadAfterRail = 2;  // the read before the reset release
    assert(freeink::papermono::enableTouch());
    assert((expanderOut() & touchUp) == touchUp);
  } else if (!strcmp(scenario, "touch-never-verifies")) {
    assert(freeink::papermono::ensureBooted(2000));
    dropTouchReset = true;
    const unsigned long start = hostMillis;
    assert(!freeink::papermono::enableTouch());
    assert(hostMillis - start < 1200);  // bounded
  } else {
    fprintf(stderr, "unknown scenario %s\n", scenario);
    return 2;
  }
  printf("Paper Mono bring-up %s PASS\n", scenario);
  return 0;
}
