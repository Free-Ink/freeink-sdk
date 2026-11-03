#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define HIGH 1
#define PROGMEM
#define pgm_read_byte(p) (*(p))
inline unsigned long millis() {
  static unsigned long t;
  return ++t;
}
inline unsigned long fakeDelayMs = 0;
inline void delay(unsigned long ms) { fakeDelayMs += ms; }
inline int digitalRead(int) { return 0; }
