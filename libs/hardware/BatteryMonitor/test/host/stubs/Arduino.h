#pragma once
#include <cstdint>
constexpr int HIGH = 1, LOW = 0, INPUT = 1, OUTPUT = 3, INPUT_PULLUP = 5, INPUT_PULLDOWN = 9;
inline unsigned long hostMillis = 0;  // the test's clock; delay() advances it
inline unsigned long millis() { return hostMillis; }
inline void delay(unsigned long ms) { hostMillis += ms; }
inline void delayMicroseconds(unsigned int) {}
inline unsigned hostPinModeCalls = 0;
inline void pinMode(int, int) { ++hostPinModeCalls; }
inline void digitalWrite(int, int) {}
inline int hostDigitalPin = 0;  // the test's detect-pin level
inline unsigned hostDigitalReadCalls = 0;
inline int digitalRead(int) {
  ++hostDigitalReadCalls;
  return hostDigitalPin;
}
inline uint32_t hostAdcMv = 0;  // the test's battery pin reading
inline unsigned hostAdcReads = 0;
inline const uint32_t* hostAdcSequence = nullptr;
inline unsigned hostAdcSequenceSize = 0;
inline uint32_t analogReadMilliVolts(int) {
  const unsigned index = hostAdcReads++;
  return hostAdcSequence && index < hostAdcSequenceSize ? hostAdcSequence[index] : hostAdcMv;
}
struct SerialStub {
  explicit operator bool() const { return false; }
  template <typename... Args>
  void printf(const char*, Args...) {}
};
inline SerialStub Serial;
