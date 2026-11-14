#pragma once
// Host stub: a virtual clock (delay advances it instantly) and the small
// String/IPAddress surface SecureHttpClient touches.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

inline unsigned long& fakeNow() {
  static unsigned long now = 0;
  return now;
}
inline unsigned long millis() { return fakeNow(); }
inline void delay(unsigned long ms) { fakeNow() += ms; }

class String {
 public:
  String(const char* s = "") : s_(s) {}
  const char* c_str() const { return s_.c_str(); }

 private:
  std::string s_;
};

class IPAddress {
 public:
  explicit IPAddress(const char* host = "") : host_(host) {}
  const char* c_str() const { return host_.c_str(); }

 private:
  std::string host_;
};
