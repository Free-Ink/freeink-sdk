#pragma once
#include <mutex>
#include <freertos/FreeRTOS.h>
using SemaphoreHandle_t = std::mutex*;
constexpr unsigned portMAX_DELAY = ~0u;
inline bool failMutex = false;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return failMutex ? nullptr : new std::mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned) { mutex->lock(); return pdTRUE; }
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
