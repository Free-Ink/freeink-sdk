#pragma once

#include <string>

#include "Arduino.h"

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  bool reserve(unsigned n) {
    std::string::reserve(n);
    return true;
  }
  bool concat(const char* s, unsigned n) {
    append(s, n);
    return true;
  }
  bool endsWith(const char* suffix) const {
    const size_t count = std::char_traits<char>::length(suffix);
    return size() >= count && compare(size() - count, count, suffix) == 0;
  }
};
