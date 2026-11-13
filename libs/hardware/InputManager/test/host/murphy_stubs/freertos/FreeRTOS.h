#pragma once

#include <cstdint>

using QueueHandle_t = void*;
using TaskHandle_t = void*;
using TickType_t = uint32_t;
using StackType_t = uint32_t;
struct StaticTask_t {};
using portMUX_TYPE = int;
constexpr int pdTRUE = 1;
constexpr int configMINIMAL_STACK_SIZE = 128;
#define pdMS_TO_TICKS(ms) (ms)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))