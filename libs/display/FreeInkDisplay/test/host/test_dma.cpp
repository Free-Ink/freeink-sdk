#include <BoardConfig.h>
#include <esp_heap_caps.h>

#include <cassert>
#include <cstdio>

#include "EpdBus.h"

using freeink::EpdBus;
const freeink::EpdPins pins{0, 1, 2, 3, 4, 5};

void reset() {
  assert(busCount == 0 && deviceCount == 0 && liveDmaAllocations == 0 && busLocks == 0);
  arduinoBusStarted = false;
  pioTransfers = 0;
  failDmaAllocation = failBusInit = failAddDevice = failTransmit = false;
  BoardConfig::ACTIVE.sd.cs = -1;
  BoardConfig::ACTIVE.sd.separateSpi = false;
  BoardConfig::ACTIVE.sdmmc.busWidth = 4;
  dmaWrites.clear();
}

int main() {
  reset();
  {
    EpdBus bus;
    bus.begin(pins, 10000000, freeink::BusyPolarity::UcIdleHigh);
    std::vector<uint8_t> source(10000);
    for (size_t i = 0; i < source.size(); ++i) source[i] = i * 73;
    {
      auto transaction = bus.beginTxn();
      transaction.cmd(0x24);
      for (size_t offset = 0; offset < source.size(); offset += 100) {
        uint8_t row[100];
        memcpy(row, source.data() + offset, sizeof(row));
        transaction.writeBytes(row, sizeof(row));
        memset(row, 0xee, sizeof(row));  // Caller storage can be reused immediately.
      }
      auto moved = std::move(transaction);
      moved.cmd(0x12);  // Must flush the tail while DC is still HIGH.
      moved.data(0xab);
    }
    assert(dmaWrites.size() == 6 && pioTransfers == 0);
    assert(dmaWrites[0].dc == LOW && dmaWrites[0].bytes == std::vector<uint8_t>{0x24});
    std::vector<uint8_t> actual;
    for (int i = 1; i <= 3; ++i) {
      assert(dmaWrites[i].dc == HIGH);
      actual.insert(actual.end(), dmaWrites[i].bytes.begin(), dmaWrites[i].bytes.end());
    }
    assert(actual == source);
    assert(dmaWrites[4].dc == LOW && dmaWrites[4].bytes == std::vector<uint8_t>{0x12});
    assert(dmaWrites[5].dc == HIGH && dmaWrites[5].bytes == std::vector<uint8_t>{0xab});
    assert(gpioLevels[2] == HIGH && busLocks == 0);
    const uint8_t data[] = {2, 3, 4};
    bus.cmdData(0x32, data, sizeof(data));
    bus.data(data, sizeof(data));
    assert(dmaWrites.back().bytes == std::vector<uint8_t>({2, 3, 4}));
    failTransmit = true;
    bus.cmdData(0x32, data, sizeof(data));
    assert(gpioLevels[2] == HIGH && busLocks == 0);
    failTransmit = false;
    bus.begin(pins, 10000000, freeink::BusyPolarity::UcIdleHigh);
    assert(busCount == 1 && deviceCount == 1 && liveDmaAllocations == 1);
  }
  for (int failure = 0; failure < 6; ++failure) {
    reset();
    failDmaAllocation = failure == 0;
    failBusInit = failure == 1;
    failAddDevice = failure == 2;
    if (failure == 3) {
      BoardConfig::ACTIVE.sd.cs = 10;
      BoardConfig::ACTIVE.sdmmc.busWidth = 0;
    }
    if (failure == 4) arduinoBusStarted = true;
    {
      EpdBus bus;
      bus.begin(pins, 10000000, freeink::BusyPolarity::UcIdleHigh, -1, failure == 5 ? 10 : -1);
      bus.cmdData2(0x32, 0xaa, 0xbb);
      assert(pioTransfers > 0 && dmaWrites.empty());
      assert(busCount == 0 && liveDmaAllocations == 0);
    }
  }
  reset();
  puts("EpdBus DMA batching, framing, lifetime, setup fallback and failure tests passed");
}
