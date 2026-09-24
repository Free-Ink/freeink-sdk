#pragma once
using gpio_num_t = int;
void gpio_hold_dis(gpio_num_t);
inline void gpio_hold_en(gpio_num_t) {}
