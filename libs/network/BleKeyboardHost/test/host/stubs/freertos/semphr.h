#pragma once

// Host-test stand-in for <freertos/semphr.h>: a binary semaphore on a mutex and a
// condition variable, so a give from one thread wakes a take on another.

#include <freertos/FreeRTOS.h>
#include <stdint.h>

struct FakeSemaphore;
typedef FakeSemaphore* SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateBinary();
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticksToWait);
