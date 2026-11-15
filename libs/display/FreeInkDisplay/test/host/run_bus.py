#!/usr/bin/env python3
"""Check production EpdBus waits and S3 DMA framing with deterministic hardware shims."""
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
#include <vector>
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
inline int gpioLevels[64] = {};
inline void digitalWrite(int pin, int level) { gpioLevels[pin] = level; }
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
inline bool arduinoBusStarted = false;
inline unsigned pioTransfers = 0;
struct FakeSPI {
 void* bus() { return arduinoBusStarted ? this : nullptr; }
 void begin(int,int,int,int) { arduinoBusStarted = true; }
 void beginTransaction(SPISettings) {}
 void endTransaction() {}
 uint8_t transfer(uint8_t v) { ++pioTransfers; return v; }
 void writeBytes(const uint8_t*, uint16_t) { ++pioTransfers; }
};
inline FakeSPI SPI;
''')
    (root / "BoardConfig.h").write_text('''#pragma once
namespace BoardConfig {
struct Sd { int cs = -1; bool separateSpi = false; };
struct Sdmmc { int busWidth = 4; };
struct Board { Sd sd; Sdmmc sdmmc; };
inline Board ACTIVE;
inline bool isOnePage() { return false; }
}
''')
    (root / "driver/gpio.h").write_text('''#pragma once
using gpio_num_t = int;
inline void gpio_hold_dis(gpio_num_t) {}
''')
    (root / "freertos/FreeRTOS.h").write_text('''#pragma once
using BaseType_t = int;
#define pdFALSE 0
#define portMAX_DELAY 0xffffffff
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
    (root / "esp_heap_caps.h").write_text('''#pragma once
#include <cstdlib>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
inline bool failDmaAllocation = false;
inline int liveDmaAllocations = 0;
inline void* heap_caps_malloc(size_t bytes, int) {
 if (failDmaAllocation) return nullptr;
 ++liveDmaAllocations;
 return std::malloc(bytes);
}
inline void heap_caps_free(void* ptr) {
 if (ptr) --liveDmaAllocations;
 std::free(ptr);
}
''')
    (root / "driver/spi_master.h").write_text('''#pragma once
#include <Arduino.h>
#define ESP_OK 0
#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3
#define SPI_TRANS_USE_TXDATA 1
#define SPI_TRANS_USE_RXDATA 2
using spi_device_handle_t = void*;
struct spi_bus_config_t {
 int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz;
};
struct spi_device_interface_config_t { int clock_speed_hz, mode, spics_io_num, queue_size; };
struct spi_transaction_t {
 unsigned flags = 0; size_t length = 0; const void* tx_buffer = nullptr;
 uint8_t tx_data[4] = {}, rx_data[4] = {};
};
struct DmaWrite { int dc; std::vector<uint8_t> bytes; };
inline std::vector<DmaWrite> dmaWrites;
inline bool failBusInit = false, failAddDevice = false, failTransmit = false;
inline int busCount = 0, deviceCount = 0, busLocks = 0;
inline int spi_bus_initialize(int host, const spi_bus_config_t* bus, int dma) {
 if (host != SPI2_HOST || dma != SPI_DMA_CH_AUTO || bus->max_transfer_sz != 4096) std::abort();
 if (failBusInit) return -1;
 ++busCount; return ESP_OK;
}
inline int spi_bus_free(int) { --busCount; return ESP_OK; }
inline int spi_bus_add_device(int, const spi_device_interface_config_t* device, spi_device_handle_t* handle) {
 if (device->clock_speed_hz != 10000000 || device->mode || device->spics_io_num != -1) std::abort();
 if (failAddDevice) return -1;
 *handle = &deviceCount; ++deviceCount; return ESP_OK;
}
inline int spi_bus_remove_device(spi_device_handle_t) { --deviceCount; return ESP_OK; }
inline int spi_device_acquire_bus(spi_device_handle_t, unsigned) { ++busLocks; return ESP_OK; }
inline void spi_device_release_bus(spi_device_handle_t) { --busLocks; }
inline int spi_device_transmit(spi_device_handle_t, spi_transaction_t* transaction) {
 if (busLocks != 1 || gpioLevels[2] != LOW || transaction->length > 4096 * 8) std::abort();
 if (failTransmit) return -1;
 const auto* data = transaction->flags & SPI_TRANS_USE_TXDATA ? transaction->tx_data
                  : static_cast<const uint8_t*>(transaction->tx_buffer);
 dmaWrites.push_back({gpioLevels[3], {data, data + transaction->length / 8}});
 transaction->rx_data[0] = 0x5a;
 return ESP_OK;
}
''')
    exe = root / "test_bus"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-I"+str(root), "-I"+str(LIB / "src/bus"), str(HERE / "test_bus.cpp"),
                    str(LIB / "src/bus/EpdBus.cpp"), "-o", str(exe)], check=True)
    for mode in ("isr", "hook", "fallback"):
        subprocess.run([str(exe), mode], check=True)
    dma_exe = root / "test_dma"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    "-DARDUINO", "-DBOARD_HAS_PSRAM", "-DCONFIG_IDF_TARGET_ESP32S3",
                    "-I"+str(root), "-I"+str(LIB / "src/bus"), str(HERE / "test_dma.cpp"),
                    str(LIB / "src/bus/EpdBus.cpp"), "-o", str(dma_exe)], check=True)
    subprocess.run([str(dma_exe)], check=True)
