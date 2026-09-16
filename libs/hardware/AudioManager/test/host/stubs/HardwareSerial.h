#pragma once
#include <cstddef>
#include <cstring>
#include <string>
#define SERIAL_8N1 0
inline std::string uartCommands;
inline int uartBegins = 0, uartEnds = 0;
inline bool uartFail = false;
class HardwareSerial {
 public:
  explicit HardwareSerial(int n) { if (n != 2) __builtin_abort(); }
  void begin(int baud, int, int rx, int tx) {
    if (baud != 115200 || rx != 47 || tx != 48) __builtin_abort();
    ++uartBegins;
  }
  size_t print(const char* text) { uartCommands += text; return uartFail ? 0 : strlen(text); }
  void flush() {}
  void end() { ++uartEnds; }
};
