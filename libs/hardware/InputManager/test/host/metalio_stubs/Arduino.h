#pragma once
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <climits>
#include <cstring>
#include <cstdlib>
#define IRAM_ATTR
constexpr int HIGH=1, LOW=0, INPUT=1, OUTPUT=3, INPUT_PULLUP=5, INPUT_PULLDOWN=6, FALLING=2, ADC_11db=3;
inline unsigned long fakeNow=1000;
inline int gpioLevels[49]={};
inline void (*touchIsr)()=nullptr;
inline void pinMode(int,int) {}
inline void digitalWrite(int p,int v) { gpioLevels[p]=v; }
inline int digitalRead(int p) { return gpioLevels[p]; }
inline void delay(unsigned long n) { fakeNow+=n; }
inline void delayMicroseconds(unsigned int) {}
inline unsigned long millis() { return fakeNow; }
inline void analogSetAttenuation(int) {}
inline int analogReadMilliVolts(int) { return 3300; }
inline int analogRead(int) { return 4095; }
inline int digitalPinToInterrupt(int p) { return p; }
inline void attachInterrupt(int,void (*f)(),int) { touchIsr=f; }
struct SerialStub { explicit operator bool() const { return false; } };
inline SerialStub Serial;
