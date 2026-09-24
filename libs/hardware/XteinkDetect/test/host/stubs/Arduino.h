#pragma once
#include <cstdint>
#include <cstddef>
#include <initializer_list>
constexpr int HIGH=1, LOW=0, INPUT=1, OUTPUT=3, INPUT_PULLUP=5;
void pinMode(int,int);
void digitalWrite(int,int);
int digitalRead(int);
void delay(unsigned long);
void delayMicroseconds(unsigned int);
unsigned long millis();
struct SerialStub {
  explicit operator bool() const { return false; }
  template<typename... Args> void printf(const char*, Args...) {}
};
inline SerialStub Serial;
