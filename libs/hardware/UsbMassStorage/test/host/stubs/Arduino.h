#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

struct FakeSerial {
  std::string output;
  explicit operator bool() const { return true; }
  int printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    const int size = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (size > 0) output.append(buffer, std::min(static_cast<size_t>(size), sizeof(buffer) - 1));
    return size;
  }
};
inline FakeSerial Serial;
