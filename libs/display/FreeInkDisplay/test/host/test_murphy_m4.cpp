#include "src/driver/Ssd1677Driver.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

bool hasWrite(const freeink::EpdBus& bus, const uint8_t command, const std::initializer_list<uint8_t> data) {
  for (const auto& write : bus.writes) {
    if (write.command == command && write.bytes == std::vector<uint8_t>(data)) return true;
  }
  return false;
}

void checkBatch(const std::string& batchName) {
  const bool firstBatch = batchName == "first";
  assert(firstBatch || batchName == "second");
  const auto batch = firstBatch ? freeink::MurphyM4Batch::First : freeink::MurphyM4Batch::Second;
  const uint8_t expectedTemperature = firstBatch ? 0x3C : 0x50;

  freeink::EpdBus bus;
  auto& driver = freeink::ssd1677Driver(batch);
  std::vector<uint8_t> frame(48000, 0xA5);
  std::vector<uint8_t> previous(48000, 0x5A);
  driver.begin(bus);
  driver.display(bus, frame.data(), previous.data(), freeink::RefreshMode::Full, false);

  bus.clear();
  driver.displayWindow(bus, frame.data(), previous.data(), 16, 20, 16, 4, false);
  assert(hasWrite(bus, 0x1A, {expectedTemperature}));
  assert(hasWrite(bus, 0x22, {0xFC}));

  bus.clear();
  driver.display(bus, frame.data(), previous.data(), freeink::RefreshMode::Half, false);
  assert(hasWrite(bus, 0x1A, {expectedTemperature}));
  assert(hasWrite(bus, 0x22, {0xD4}));
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  checkBatch(argv[1]);
}