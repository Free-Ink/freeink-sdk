#include <cassert>
#include <cstdio>

#include "EpdBus.h"

// Supply board escape hatches for hosts without ELF weak-reference semantics.
extern "C" void freeink_board_epd_power(bool) {}
extern "C" void freeink_board_epd_reset(bool) {}

using freeink::BusyPolarity;
using freeink::EpdBus;
static unsigned begins, ends, slices;
static void beginWait() { ++begins; }
static void endWait() { ++ends; }
static bool slice(int8_t pin, uint8_t level) {
  assert(pin == 5 && level == busyLevel);
  ++slices;
  delay(1);
  return true;
}
static void schedule(uint64_t start, uint64_t end, int level = LOW) {
  nowUs = 0;
  busyStartUs = start;
  busyEndUs = end;
  busyLevel = level;
  token = false;
  begins = ends = slices = 0;
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const bool hooked = !strcmp(argv[1], "hook");
  const bool fallback = !strcmp(argv[1], "fallback");
  allocateSemaphore = !fallback;
  EpdBus bus;
  bus.begin({0, 1, 2, 3, 4, 5}, 20000000, BusyPolarity::ActiveLowReady);
  assert(busyMode == INPUT_PULLUP);
  bus.setBusyWaitHooks(beginWait, endWait);
  if (hooked) bus.setBusyWaitSliceHook(slice);

  schedule(0, 0);
  assert(!bus.isBusy());
  bus.waitBusy();
  assert(millis() == 1 && begins == 0 && ends == 0);
  // Other active-low boards retain their assertion grace.
  schedule(0, 0);
  bus.waitBusy(BusyPolarity::ActiveLow);
  assert(millis() == 100);

  // Catch a BUSY assertion after the command but within its settling guard.
  schedule(500, 45000);
  bus.waitBusy();
  assert(nowUs >= 45000 && nowUs <= 55000);
  assert(begins == 1 && ends == 1);
  assert(!hooked || slices > 0);
  schedule(0, 45000);
  assert(bus.isBusy());
  bus.waitBusy();
  assert(!bus.isBusy() && begins == ends);

  // A stuck controller still has the existing bounded wait and balanced hooks.
  schedule(0, UINT64_MAX);
  bus.waitBusy();
  assert(millis() > 30000 && millis() <= 30011);
  assert(begins == 1 && ends == 1);

  // Refresh assertion is deliberately later than the command settling guard.
  // ISR, hooked polling, and semaphore-allocation fallback must all wait for it.
  schedule(5000, 85000);
  bus.waitRefreshComplete();
  assert(nowUs >= 85000 && nowUs <= 95000);
  assert(begins == 1 && ends == 1 && isr == nullptr);
  assert(!hooked || slices > 0);
  schedule(0, UINT64_MAX);
  bus.waitRefreshComplete();
  assert(millis() >= 30000 && millis() <= 30010);
  assert(begins == 1 && ends == 1 && isr == nullptr);
  schedule(0, 0);
  bus.waitRefreshComplete();
  assert(millis() == (hooked || fallback ? 100 : 20));
  assert(begins == 0 && ends == 0 && isr == nullptr);

  // SSD1677's electrical polarity and immediate ready behavior are unchanged.
  bus.begin({0, 1, 2, 3, 4, 5}, 20000000, BusyPolarity::ActiveHigh);
  assert(busyMode == INPUT);
  schedule(0, 0, HIGH);
  bus.waitBusy();
  assert(millis() == 0 && !bus.isBusy());
  schedule(0, 45000, HIGH);
  assert(bus.isBusy());
  bus.waitBusy();
  assert(millis() == 45 && begins == 1 && ends == 1);
  std::printf("PASS: production bus command/refresh waits (%s)\n", argv[1]);
}
