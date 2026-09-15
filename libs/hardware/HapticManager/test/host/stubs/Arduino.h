#pragma once
#include "../../../../InputManager/test/host/metalio_stubs/Arduino.h"

inline bool pwmAttached = false, failAttach = false, failWrite = false;
inline unsigned pwmWrites = 0;
inline uint8_t pwmPin = 0;
inline uint32_t pwmDuty = 0, pwmFrequency = 0;
inline bool ledcAttach(uint8_t pin, uint32_t hz, uint8_t bits) {
  if (failAttach || bits != 8) return false;
  pwmAttached = true; pwmPin = pin; pwmFrequency = hz; pwmDuty = 0;
  return true;
}
inline bool ledcWrite(uint8_t pin, uint32_t duty) {
  ++pwmWrites;
  if (!pwmAttached || failWrite || pin != pwmPin) return false;
  pwmDuty = duty;
  return true;
}
inline bool ledcDetach(uint8_t) { pwmAttached = false; return true; }
