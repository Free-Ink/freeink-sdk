#pragma once
using gpio_num_t=int;
inline int gpio_set_level(gpio_num_t, unsigned int) { return 0; }
inline void gpio_hold_dis(int) {}
inline void gpio_hold_en(int) {}
inline void gpio_set_level(int, int) {}
