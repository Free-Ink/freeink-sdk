#pragma once
#include <cstdint>
struct USBMSC {
  inline static int32_t (*read)(uint32_t, uint32_t, void*, uint32_t) = nullptr;
  inline static int32_t (*write)(uint32_t, uint32_t, uint8_t*, uint32_t) = nullptr;
  inline static bool (*startStop)(uint8_t, bool, bool) = nullptr;
  inline static unsigned ends = 0;
  void vendorID(const char*) {}
  void productID(const char*) {}
  void productRevision(const char*) {}
  void mediaPresent(bool) {}
  void isWritable(bool) {}
  void onStartStop(decltype(startStop) callback) { startStop = callback; }
  void onRead(decltype(read) callback) { read = callback; }
  void onWrite(decltype(write) callback) { write = callback; }
  bool begin(uint32_t, uint16_t) { return true; }
  void end() {
    ++ends;
    read = nullptr;
    write = nullptr;
    startStop = nullptr;
  }
};
