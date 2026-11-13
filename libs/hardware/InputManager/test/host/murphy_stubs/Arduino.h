#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#define IRAM_ATTR
constexpr int HIGH = 1;
constexpr int LOW = 0;
constexpr int INPUT = 1;
constexpr int OUTPUT = 3;
constexpr int INPUT_PULLUP = 5;
constexpr int INPUT_PULLDOWN = 6;
constexpr int ADC_11db = 3;

inline unsigned long fakeNow = 1000;
inline int fakePinLevels[64] = {};
inline int fakePinModes[64] = {};
inline void pinMode(const int pin, const int mode) { fakePinModes[pin] = mode; }
inline void digitalWrite(const int pin, const int value) { fakePinLevels[pin] = value; }
inline int digitalRead(const int pin) { return fakePinLevels[pin]; }
inline void delay(const unsigned long ms) { fakeNow += ms; }
inline void delayMicroseconds(unsigned int) {}
inline unsigned long millis() { return fakeNow; }
inline void analogSetAttenuation(int) {}
inline int analogRead(int) { return 4095; }
inline int analogReadMilliVolts(int) { return 3300; }
inline int digitalPinToInterrupt(const int pin) { return pin; }
inline void attachInterrupt(int, void (*)(), int) {}
inline void detachInterrupt(int) {}

struct SerialStub {
  explicit operator bool() const { return false; }
  template <class... Args>
  void printf(const char*, Args...) {}
  void print(const char*) {}
  size_t write(const uint8_t*, size_t length) { return length; }
};
inline SerialStub Serial;