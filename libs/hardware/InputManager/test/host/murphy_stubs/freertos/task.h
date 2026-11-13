#pragma once

#include "FreeRTOS.h"

inline int fakeStaticTaskCreateCalls = 0;
inline void vTaskDelay(TickType_t) {}
inline void vTaskDelayUntil(TickType_t*, TickType_t) {}
inline TickType_t xTaskGetTickCount() { return 0; }
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, TaskHandle_t*) { return 0; }
inline TaskHandle_t xTaskCreateStaticPinnedToCore(void (*)(void*), const char*, uint32_t, void*, int, StackType_t*,
                                                 StaticTask_t*, int) {
  ++fakeStaticTaskCreateCalls;
  return nullptr;
}