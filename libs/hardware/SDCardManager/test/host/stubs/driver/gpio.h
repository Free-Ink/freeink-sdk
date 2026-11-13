#pragma once
using gpio_num_t = int;
inline void gpio_hold_dis(gpio_num_t) {}
constexpr int GPIO_MODE_INPUT = 0;
inline unsigned floatedPins = 0;
inline void gpio_set_direction(gpio_num_t, int) { ++floatedPins; }
inline void gpio_pullup_dis(gpio_num_t) {}
inline void gpio_pulldown_dis(gpio_num_t) {}
