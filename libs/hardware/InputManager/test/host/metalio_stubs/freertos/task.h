#pragma once
#include "FreeRTOS.h"
inline void vTaskDelay(int) {}
inline int xTaskCreate(void (*)(void*),const char*,int,void*,int,void**) { return 0; }
// Runs the task body to completion before returning, so host tests see a
// background start finish deterministically.
inline int xTaskCreatePinnedToCore(void (*fn)(void*),const char*,int,void* arg,int,void**,int) { fn(arg); return pdPASS; }
inline void vTaskDelete(void*) {}
