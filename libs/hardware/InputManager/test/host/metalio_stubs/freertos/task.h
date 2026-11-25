#pragma once
#include "FreeRTOS.h"
#include <chrono>
#include <thread>
inline bool fakeTaskEnabled = false;
inline void vTaskDelay(int ms) {
  if (fakeTaskEnabled) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
inline int xTaskCreate(void (*fn)(void*),const char*,int,void* arg,int,void**) {
  if (!fakeTaskEnabled) return 0;
  std::thread([=] { fn(arg); }).detach();
  return pdPASS;
}
// Runs the task body to completion before returning, so host tests see a
// background start finish deterministically.
inline int xTaskCreatePinnedToCore(void (*fn)(void*),const char*,int,void* arg,int,void**,int) { fn(arg); return pdPASS; }
inline void vTaskDelete(void*) {}
