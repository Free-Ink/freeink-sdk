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
constexpr uint8_t m5pm1Button = freeink::m5pm1::REG_BTN_STATUS;
std::array<std::array<uint8_t, 256>, 128> regs{};
unsigned long expanderUpAt = 0;  // the expander NACKs until hostMillis reaches this
int failExpanderReadAfterRail = 0;  // fail the Nth expander read after TP_EN first goes high
int failExpanderWriteAt = 0;        // fail the Nth expander write (counted from 1)
int expanderWrites = 0;
bool dropTouchReset = false;        // the expander ignores TP_RST writes
bool railSeen = false;
int readsAfterRail = 0;
int touchResetReleases = 0;
int busOperations = 0;

bool isExpander(uint8_t addr) { return addr == ioe1::ADDR_PRIMARY || addr == ioe1::ADDR_ALT; }

uint16_t expanderMode() {
  const auto& r = regs[ioe1::ADDR_PRIMARY];
  return static_cast<uint16_t>(r[ioe1::REG_GPIO_MODE_L] | (r[ioe1::REG_GPIO_MODE_L + 1] << 8));
}

uint16_t expanderOut() {
  const auto& r = regs[ioe1::ADDR_PRIMARY];
  return static_cast<uint16_t>(r[ioe1::REG_GPIO_OUT_L] | (r[ioe1::REG_GPIO_OUT_L + 1] << 8));
}
}  // namespace

bool hostI2cWrite(uint8_t addr, const std::vector<uint8_t>& bytes) {
  ++busOperations;
  if (isExpander(addr) && hostMillis < expanderUpAt) return false;
  if (bytes.empty()) return true;
  if (isExpander(addr) && ++expanderWrites == failExpanderWriteAt) return false;
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
    assert(freeink::papermono::ensureBooted());
    assert(hostMillis == 0);
    assert(freeink::papermono::bootWakeState().valid && freeink::papermono::bootWakeState().source == 0x20);
    assert(regs[PM1][0x05] == 0);
    assert(freeink::papermono::enableTouch());
    assert((expanderOut() & touchUp) == touchUp && touchResetReleases == 1);
  } else if (!strcmp(scenario, "late-expander")) {
    // Seen after a PMIC restart: the expander misses the first probes. One probe used to
    // mark it absent for the session.
    expanderUpAt = 600;
    assert(freeink::papermono::ensureBooted());
    assert(hostMillis >= 600 && hostMillis < 700);
    const uint16_t mode = regs[ioe1::ADDR_PRIMARY][ioe1::REG_GPIO_MODE_L] |
                          (regs[ioe1::ADDR_PRIMARY][ioe1::REG_GPIO_MODE_L + 1] << 8);
    assert((mode & ioe1::OUTPUT_MASK) == ioe1::OUTPUT_MASK);
    assert(freeink::papermono::bootWakeState().source == 0x20);  // snapshot from the first attempt
    assert(freeink::papermono::enableTouch());
  } else if (!strcmp(scenario, "missing-expander")) {
    expanderUpAt = 1000000;
    assert(!freeink::papermono::ensureBooted());
    assert(hostMillis >= 2000 && hostMillis < 2100);
    // Cached as before: no late configureOutputs() that would cut rails in use.
    expanderUpAt = 0;
    const int operations = busOperations;
    assert(!freeink::papermono::ensureBooted());
    assert(!freeink::papermono::enableTouch());
    assert(busOperations == operations);
  } else if (!strcmp(scenario, "partial-setup-failure")) {
    // One configureOutputs() write fails: the failure is cached, a caller raises its rail
    // anyway, and no later call may rerun the setup and drive that rail LOW again.
    failExpanderWriteAt = 3;  // inside configureOutputs(), after the probe's I2C_CFG write
    assert(!freeink::papermono::ensureBooted());
    freeink::papermono::setSdPower(true);
    assert(expanderOut() & ioe1::PIN_SD_POWER);
    assert(!freeink::papermono::ensureBooted());
    regs[PM1][m5pm1Button] = 0x80;  // a click the PMIC latched
    assert(freeink::papermono::pollPowerButtonClicked(10000));  // the power button still works
    assert(expanderOut() & ioe1::PIN_SD_POWER);
  } else if (!strcmp(scenario, "sd-first-late-expander")) {
    // Whichever manager calls first gets the boot wait, not only InputManager.
    expanderUpAt = 600;
    freeink::papermono::setSdPower(true);
    assert(hostMillis >= 600 && hostMillis < 700);
    assert(expanderOut() & ioe1::PIN_SD_POWER);
    assert(freeink::papermono::ensureBooted() && freeink::papermono::enableTouch());
  } else if (!strcmp(scenario, "touch-mode-repair")) {
    // The MODE write of configureOutputs() fails: the touch pins stay inputs until
    // enableTouch() makes them outputs, without changing the other pins' direction.
    failExpanderWriteAt = 8;
    assert(!freeink::papermono::ensureBooted());
    assert(expanderMode() == 0);
    assert(freeink::papermono::enableTouch());
    assert(expanderMode() == touchUp && (expanderOut() & touchUp) == touchUp);
  } else if (!strcmp(scenario, "failed-read-during-touch")) {
    // Seen on the device: OUT 0x1000 (rail on, reset low) after one failed read in the
    // read-modify-write that releases the reset.
    assert(freeink::papermono::ensureBooted());
    failExpanderReadAfterRail = 3;  // reset-low, mode, then this one: the read before the reset release
    assert(freeink::papermono::enableTouch());
    assert((expanderOut() & touchUp) == touchUp);
  } else if (!strcmp(scenario, "touch-never-verifies")) {
    assert(freeink::papermono::ensureBooted());
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
