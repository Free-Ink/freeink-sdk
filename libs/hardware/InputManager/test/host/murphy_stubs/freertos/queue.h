#pragma once

#include "FreeRTOS.h"

inline QueueHandle_t xQueueCreate(int, int) { return nullptr; }
inline int xQueueSend(QueueHandle_t, const void*, int) { return 0; }
inline int xQueueReceive(QueueHandle_t, void*, int) { return 0; }
inline void xQueueReset(QueueHandle_t) {}