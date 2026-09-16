#pragma once
#include "../../../../InputManager/test/host/metalio_stubs/Arduino.h"
using TaskHandle_t = void*;
constexpr int pdPASS = 1;
#define pdMS_TO_TICKS(ms) (ms)
#define log_e(...) ((void)0)
inline void (*pendingTask)(void*) = nullptr;
inline void* pendingArg = nullptr;
inline bool failTask = false;
inline int xTaskCreatePinnedToCore(void (*fn)(void*), const char*, int, void* arg, int, TaskHandle_t* task, int) {
  if (failTask) return 0;
  pendingTask = fn; pendingArg = arg; *task = arg; return pdPASS;
}
inline void runTask() { auto fn = pendingTask; pendingTask = nullptr; fn(pendingArg); }
inline void vTaskDelay(int) { if (pendingTask) runTask(); }
inline void vTaskDelete(void*) {}
