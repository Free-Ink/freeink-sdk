#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>

#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#include "UsbMassStorage.h"

std::atomic<bool> mounted{true};
std::atomic<bool> suspended{false};
std::atomic<unsigned> disconnects{0};
extern "C" bool tud_mounted() { return mounted; }
extern "C" bool tud_suspended() { return suspended; }
extern "C" bool tud_disconnect() {
  ++disconnects;
  return true;
}

struct Card final : FsBlockDeviceInterface {
  std::array<uint8_t, 2048> data{};
  std::mutex mutex;
  std::condition_variable changed;
  bool holdWrite = false;
  bool writing = false;
  bool released = false;
  bool syncOk = true;
  bool readOk = true;
  bool writeOk = true;
  unsigned reads = 0;
  unsigned writes = 0;
  unsigned syncs = 0;
  unsigned sequence = 0;
  unsigned lastWriteSequence = 0;
  unsigned lastSyncSequence = 0;
  Sector_t sectors = 4;
  Sector_t lastWriteSector = 0;
  size_t lastWriteCount = 0;
  bool readSector(Sector_t sector, uint8_t* buffer) override { return readSectors(sector, buffer, 1); }
  bool readSectors(Sector_t sector, uint8_t* buffer, size_t count) override {
    ++reads;
    if (!readOk) return false;
    assert((sector + count) * 512 <= data.size());
    std::memcpy(buffer, data.data() + sector * 512, count * 512);
    return true;
  }
  bool writeSector(Sector_t sector, const uint8_t* buffer) override { return writeSectors(sector, buffer, 1); }
  bool writeSectors(Sector_t sector, const uint8_t* buffer, size_t count) override {
    std::unique_lock<std::mutex> lock(mutex);
    ++writes;
    lastWriteSequence = ++sequence;
    if (!writeOk) return false;
    writing = true;
    changed.notify_all();
    if (holdWrite) changed.wait(lock, [&] { return released; });
    assert((sector + count) * 512 <= data.size());
    std::memcpy(data.data() + sector * 512, buffer, count * 512);
    lastWriteSector = sector;
    lastWriteCount = count;
    writing = false;
    return true;
  }
  Sector_t sectorCount() override { return sectors; }
  bool syncDevice() override {
    std::lock_guard<std::mutex> lock(mutex);
    assert(!writing);
    ++syncs;
    lastSyncSequence = ++sequence;
    return syncOk;
  }
};

int main() {
  freeink::UsbMassStorage storage;
  Card card;
  USB.beginOk = false;
  assert(!storage.begin(&card));
  USB.beginOk = true;
  assert(storage.begin(&card));
  assert(storage.state() == freeink::UsbMassStorageState::Connected);
  suspended = true;
  assert(storage.hostSuspended());
  assert(storage.state() == freeink::UsbMassStorageState::Connected);
  suspended = false;
  std::array<uint8_t, 512> payload;
  payload.fill(0xA5);
  card.data.fill(0x7E);
  assert(USBMSC::write(0, 510, payload.data(), 4) == 4);
  assert(card.syncs == 1);
  for (size_t i = 0; i < card.data.size(); ++i) assert(card.data[i] == (i >= 510 && i < 514 ? 0xA5 : 0x7E));
  // An aligned USB buffer stays one multi-sector device operation with one
  // checked sync, rather than becoming separately stopped sector writes.
  std::array<uint8_t, 1024> blocks;
  blocks.fill(0xA5);
  const auto priorWrites = card.writes;
  const auto priorSyncs = card.syncs;
  assert(USBMSC::write(1, 0, blocks.data(), blocks.size()) == 1024);
  assert(card.writes == priorWrites + 1);
  assert(card.lastWriteSector == 1 && card.lastWriteCount == 2);
  assert(card.syncs == priorSyncs + 1);
  assert(card.lastSyncSequence == card.lastWriteSequence + 1);
  const auto before = card.data;
  const auto beforeReads = card.reads;
  const auto beforeWrites = card.writes;
  assert(USBMSC::write(3, 500, payload.data(), 13) == -1);
  assert(storage.state() == freeink::UsbMassStorageState::IoError);
  // A bad range intentionally closes admission, including otherwise valid
  // requests that arrive afterwards. Neither path may touch the device.
  assert(USBMSC::write(0, 0, payload.data(), payload.size()) == -1);
  assert(USBMSC::read(0, 0, payload.data(), payload.size()) == -1);
  assert(card.writes == beforeWrites && card.reads == beforeReads);
  assert(card.data == before);
  assert(storage.end());

  assert(storage.begin(&card));
  card.readOk = false;
  assert(USBMSC::read(0, 0, payload.data(), payload.size()) == -1);
  assert(storage.state() == freeink::UsbMassStorageState::IoError);
  const auto failedReads = card.reads;
  card.readOk = true;
  assert(USBMSC::read(0, 0, payload.data(), payload.size()) == -1);
  assert(card.reads == failedReads);
  assert(storage.end());

  assert(storage.begin(&card));
  card.writeOk = false;
  const auto beforeFailedWrite = card.data;
  assert(USBMSC::write(0, 0, payload.data(), payload.size()) == -1);
  assert(storage.state() == freeink::UsbMassStorageState::IoError);
  const auto failedWrites = card.writes;
  card.writeOk = true;
  assert(USBMSC::write(0, 0, payload.data(), payload.size()) == -1);
  assert(card.writes == failedWrites && card.data == beforeFailedWrite);
  assert(storage.end());

  assert(storage.begin(&card));
  card.syncOk = false;
  assert(USBMSC::write(0, 0, payload.data(), payload.size()) == -1);
  assert(storage.state() == freeink::UsbMassStorageState::IoError);
  assert(!USBMSC::startStop(0, false, true));
  assert(storage.state() != freeink::UsbMassStorageState::Ejected);
  assert(!storage.end());
  assert(storage.active());
  const auto writes = card.writes;
  assert(USBMSC::write(1, 0, payload.data(), payload.size()) == -1);
  assert(card.writes == writes);
  card.syncOk = true;
  assert(storage.end());

  assert(storage.begin(&card));
  const auto syncs = card.syncs;
  assert(USBMSC::startStop(0, false, true));
  assert(card.syncs == syncs + 1);
  assert(storage.state() == freeink::UsbMassStorageState::Ejected);
  assert(USBMSC::write(0, 0, payload.data(), payload.size()) == -1);
  assert(storage.end());

  assert(storage.begin(&card));
  card.holdWrite = true;
  std::thread writer([&] { assert(USBMSC::write(2, 0, payload.data(), payload.size()) == 512); });
  {
    std::unique_lock<std::mutex> lock(card.mutex);
    card.changed.wait(lock, [&] { return card.writing; });
  }
  std::atomic<bool> ended{false};
  const auto oldDisconnects = disconnects.load();
  std::thread stop([&] {
    assert(storage.end());
    ended = true;
  });
  while (disconnects == oldDisconnects) std::this_thread::yield();
  assert(!ended);
  std::thread lateWriter([&] { assert(USBMSC::write(3, 0, payload.data(), payload.size()) == -1); });
  {
    std::lock_guard<std::mutex> lock(card.mutex);
    card.released = true;
    card.changed.notify_all();
  }
  writer.join();
  stop.join();
  lateWriter.join();
  assert(ended);
  for (size_t i = 1024; i < 1536; ++i) assert(card.data[i] == 0xA5);
  for (size_t i = 1536; i < 2048; ++i) assert(card.data[i] == 0x7E);
  assert(USBMSC::ends == 0);
  assert(USBMSC::write(0, 0, payload.data(), payload.size()) == -1);
  assert(!USBMSC::startStop(0, false, true));
  Card differentCapacity;
  differentCapacity.sectors = 2;
  assert(!storage.begin(&differentCapacity));
  assert(!storage.active());
  assert(Serial.output.find("capacity changed (4 -> 2 sectors)") != std::string::npos);
  assert(Serial.output.find("restart before using a different-sized card") != std::string::npos);
  assert(storage.begin(&card));
  assert(storage.state() == freeink::UsbMassStorageState::Connected);
  assert(storage.end());
  std::cout << "PASS: batched checked writes, latched errors, flush retry, "
               "suspend ownership, I/O drain and capacity retry\n";
}
