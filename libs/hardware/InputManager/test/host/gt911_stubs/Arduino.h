#pragma once
// Arduino surface for the X4 Pro GT911 INT-wake host test. Records pin modes so
// the test can see whether the driver pulled the INT line up.
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#define IRAM_ATTR
constexpr int HIGH = 1, LOW = 0, INPUT = 1, OUTPUT = 3, INPUT_PULLUP = 5, INPUT_PULLDOWN = 6, FALLING = 2, ADC_11db = 3;
inline unsigned long fakeNow = 1000;
inline int gpioLevels[49] = {};
inline int gpioModes[49] = {};
inline void pinMode(int p, int m) { gpioModes[p] = m; }
inline void (*gpioRisingHook)(int) = nullptr;  // models wired to a pin (GT911 RST)
inline void digitalWrite(int p, int v) {
  const bool rising = gpioLevels[p] == LOW && v == HIGH;
  gpioLevels[p] = v;
  if (rising && gpioRisingHook) gpioRisingHook(p);
}
inline int digitalRead(int p) { return gpioLevels[p]; }
inline void delay(unsigned long n) { fakeNow += n; }
inline void delayMicroseconds(unsigned int) {}
inline unsigned long millis() { return fakeNow; }
inline void analogSetAttenuation(int) {}
inline int analogReadMilliVolts(int) { return 3300; }
inline int analogRead(int) { return 4095; }
inline int digitalPinToInterrupt(int p) { return p; }
inline void attachInterrupt(int, void (*)(), int) {}
inline void detachInterrupt(int) {}
struct SerialStub {
  explicit operator bool() const { return false; }
};
inline SerialStub Serial;
