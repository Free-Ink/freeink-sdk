#pragma once
#include <Arduino.h>
namespace freeink::m5pm1 {
constexpr uint8_t REG_GPIO_DRV = 0, REG_GPIO_FUNC0 = 1, REG_PWM_FREQ_L = 2, REG_PWM0_DUTY_L = 3;
inline void beginBus() {}
inline void updateReg(uint8_t, uint8_t, uint8_t) {}
inline void writeReg16(uint8_t, uint16_t) {}
inline void writeBytes(uint8_t, const uint8_t*, size_t) {}
}  // namespace freeink::m5pm1
