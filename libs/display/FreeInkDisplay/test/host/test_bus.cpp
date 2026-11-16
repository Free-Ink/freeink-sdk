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

  // X3TwoPhase refresh completion with _busyWaitSliceHook bounds HIGH-to-LOW
  // assertion grace to 20 ms (never 1000 ms) when BUSY already returned HIGH,
  // while still waiting cleanly via ActiveLowReady when BUSY is or goes LOW.
  bus.begin({0, 1, 2, 3, 4, 5}, 16000000, BusyPolarity::X3TwoPhase);
  bus.setBusyWaitHooks(beginWait, endWait);
  if (hooked) bus.setBusyWaitSliceHook(slice);
  schedule(0, 0, LOW);
  bus.waitRefreshComplete(" X3_DRF");
  assert((fallback ? millis() >= 1000 : millis() <= 25) && begins == 0 && ends == 0 && isr == nullptr);
  schedule(5000, 85000, LOW);
  bus.waitRefreshComplete(" X3_DRF");
  assert(nowUs >= 85000 && nowUs <= 95000 && begins == 1 && ends == 1 && isr == nullptr);
  assert(!hooked || slices > 0);
  schedule(0, 45000, LOW);
  bus.waitRefreshComplete(" X3_DRF");
  assert(nowUs >= 45000 && nowUs <= 55000 && begins == 1 && ends == 1 && isr == nullptr);

  // Verify sendPlaneFlipped and sendPlaneFlippedInverted produce 100% exact
  // bottom-up gate-order bytes and 256-byte FIFO chunks across odd & even widthBytes.
  SPI.recording = true;
  {
    SPI.clearRecord();
    bus.sendPlaneFlipped(0x13, nullptr, 10, 99);
    assert(SPI.cmdTransfers.size() == 1 && SPI.cmdTransfers[0] == 0x13);
    assert(SPI.dataBytes.empty() && SPI.chunkSizes.empty());

    const uint16_t testWidths[] = {1, 2, 3, 4, 97, 98, 99, 100, 127, 128, 255, 256, 257, 301};
    const uint16_t testHeights[] = {1, 2, 3, 5, 8, 17};
    for (const uint16_t wb : testWidths) {
      for (const uint16_t h : testHeights) {
        std::vector<uint8_t> src(static_cast<size_t>(h) * wb);
        for (size_t i = 0; i < src.size(); ++i) {
          src[i] = static_cast<uint8_t>((i * 73u + wb * 19u + h * 7u + 0x3Du) & 0xFFu);
        }
        std::vector<uint8_t> expFlip(src.size());
        std::vector<uint8_t> expInv(src.size());
        for (size_t y = 0; y < h; ++y) {
          const uint8_t* srcRow = src.data() + (h - 1u - y) * wb;
          uint8_t* dstFlip = expFlip.data() + y * wb;
          uint8_t* dstInv = expInv.data() + y * wb;
          for (size_t x = 0; x < wb; ++x) {
            dstFlip[x] = srcRow[x];
            dstInv[x] = static_cast<uint8_t>(~srcRow[x]);
          }
        }

        SPI.clearRecord();
        bus.sendPlaneFlipped(0x13, src.data(), h, wb);
        assert(SPI.cmdTransfers.size() == 1 && SPI.cmdTransfers[0] == 0x13);
        assert(SPI.dataBytes == expFlip);
        for (size_t c = 0; c + 1 < SPI.chunkSizes.size(); ++c) {
          assert(SPI.chunkSizes[c] == 256u);
        }

        SPI.clearRecord();
        bus.sendPlaneFlippedInverted(0x10, src.data(), h, wb);
        assert(SPI.cmdTransfers.size() == 1 && SPI.cmdTransfers[0] == 0x10);
        assert(SPI.dataBytes == expInv);
        for (size_t c = 0; c + 1 < SPI.chunkSizes.size(); ++c) {
          assert(SPI.chunkSizes[c] == 256u);
        }
      }
    }

    // Full X3 792x528 (99 x 528 = 52,272 B) plane verification.
    std::vector<uint8_t> x3Plane(99u * 528u);
    for (size_t i = 0; i < x3Plane.size(); ++i) {
      x3Plane[i] = static_cast<uint8_t>((i * 131u + (i >> 8) * 17u) & 0xFFu);
    }
    SPI.clearRecord();
    bus.sendPlaneFlipped(0x13, x3Plane.data(), 528, 99);
    assert(SPI.dataBytes.size() == x3Plane.size());
    assert(SPI.chunkSizes.size() == (52272u + 255u) / 256u);
    for (size_t y = 0; y < 528u; ++y) {
      assert(std::memcmp(SPI.dataBytes.data() + y * 99u, x3Plane.data() + (527u - y) * 99u, 99u) == 0);
    }
  }
  SPI.recording = false;

  std::printf("PASS: production bus command/refresh waits & plane flips (%s)\n", argv[1]);
}
