#pragma once
#include "FreeRTOS.h"
inline void vTaskDelay(int) {}
inline int xTaskCreate(void (*)(void*),const char*,int,void*,int,void**) { return 0; }
