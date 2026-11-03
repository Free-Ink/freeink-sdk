#!/usr/bin/env python3
"""Compile the production EpdBus with a deterministic clock/GPIO/RTOS shim."""
from pathlib import Path
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1]
with tempfile.TemporaryDirectory(prefix="freeink-bus-test-") as directory:
    root = Path(directory)
    (root / "driver").mkdir()
    (root / "freertos").mkdir()
    (root / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#define HIGH 1
#define LOW 0
#define INPUT 0
#define INPUT_PULLUP 2
#define OUTPUT 3
#define CHANGE 4
#define DRAM_ATTR
#define IRAM_ATTR
inline uint64_t nowUs = 0, busyStartUs = 0, busyEndUs = 0;
inline int busyLevel = LOW, busyMode = -1;
inline bool allocateSemaphore = true, token = false;
inline void (*isr)() = nullptr;
inline unsigned long millis() { return nowUs / 1000; }
inline void delay(unsigned long ms) { nowUs += ms * 1000; }
inline void delayMicroseconds(unsigned us) { nowUs += us; }
inline int digitalRead(int) {
  return nowUs >= busyStartUs && nowUs < busyEndUs ? busyLevel : !busyLevel;
}
inline void digitalWrite(int, int) {}
inline void pinMode(int pin, int mode) { if (pin == 5) busyMode = mode; }
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterrupt(int, void (*fn)(), int) { isr = fn; }
inline void detachInterrupt(int) { isr = nullptr; }
struct FakeSerial {
  explicit operator bool() const { return false; }
  template<class... T> void printf(const char*, T...) {}
};
inline FakeSerial Serial;
''')
    (root / "SPI.h").write_text('''#pragma once
#include <cstdint>
#define MSBFIRST 1
#define SPI_MODE0 0
struct SPISettings { SPISettings(uint32_t=0, int=0, int=0) {} };
struct FakeSPI {
 void begin(int,int,int,int) {}
 void beginTransaction(SPISettings) {}
 void endTransaction() {}
 uint8_t transfer(uint8_t v) { return v; }
 void writeBytes(const uint8_t*, uint16_t) {}
};
inline FakeSPI SPI;
''')
    (root / "BoardConfig.h").write_text('''#pragma once
namespace BoardConfig { inline bool isOnePage() { return false; } }
''')
    (root / "driver/gpio.h").write_text('''#pragma once
using gpio_num_t = int;
inline void gpio_hold_dis(gpio_num_t) {}
''')
    (root / "freertos/FreeRTOS.h").write_text('''#pragma once
using BaseType_t = int;
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portYIELD_FROM_ISR() ((void)0)
''')
    (root / "freertos/semphr.h").write_text('''#pragma once
#include <Arduino.h>
using SemaphoreHandle_t = void*;
inline SemaphoreHandle_t xSemaphoreCreateBinary() {
 return allocateSemaphore ? &token : nullptr;
}
inline void xSemaphoreGiveFromISR(SemaphoreHandle_t, int*) { token = true; }
inline int xSemaphoreTake(SemaphoreHandle_t, unsigned long timeout) {
 if (token) { token = false; return 1; }
 const auto deadline = nowUs + timeout * 1000;
 uint64_t edge = UINT64_MAX;
 if (busyStartUs > nowUs && busyStartUs < busyEndUs) edge = busyStartUs;
 if (busyEndUs > nowUs) edge = std::min(edge, busyEndUs);
 if (isr && edge <= deadline) {
   nowUs = edge;
   isr();
   token = false;
   return 1;
 }
 nowUs = deadline;
 return 0;
}
''')
    exe = root / "test_bus"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-I"+str(root), "-I"+str(LIB / "src/bus"), str(HERE / "test_bus.cpp"),
                    str(LIB / "src/bus/EpdBus.cpp"), "-o", str(exe)], check=True)
    for mode in ("isr", "hook", "fallback"):
        subprocess.run([str(exe), mode], check=True)
