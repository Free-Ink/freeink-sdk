#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <driver/gpio.h>
#include <esp_err.h>

constexpr int I2C_NUM_0 = 0;
constexpr int I2C_NUM_1 = 1;
constexpr int I2C_CLK_SRC_DEFAULT = 0;
constexpr int I2C_ADDR_BIT_LEN_7 = 7;

struct i2c_master_bus_config_t {
  int i2c_port = 0;
  gpio_num_t sda_io_num = 0;
  gpio_num_t scl_io_num = 0;
  int clk_source = 0;
  uint8_t glitch_ignore_cnt = 0;
  struct {
    uint8_t enable_internal_pullup = 0;
  } flags;
};

struct i2c_device_config_t {
  int dev_addr_length = 0;
  uint16_t device_address = 0;
  uint32_t scl_speed_hz = 0;
};

struct FakeI2cBus {};
struct FakeI2cDevice {
  uint8_t address = 0;
  uint32_t frequency = 0;
};
using i2c_master_bus_handle_t = FakeI2cBus*;
using i2c_master_dev_handle_t = FakeI2cDevice*;

struct FakeI2cWrite {
  uint8_t address;
  std::vector<uint8_t> bytes;
};

inline FakeI2cBus fakeI2cBus;
inline FakeI2cDevice fakeTouchDevice;
inline FakeI2cDevice fakeRtcDevice;
inline i2c_master_bus_config_t fakeI2cBusConfig;
inline int fakeI2cBusCreates = 0;
inline int fakeI2cDeviceAdds = 0;
inline std::array<uint8_t, 256> fakeTouchRegisters{};
inline std::array<uint8_t, 256> fakeRtcRegisters{};
inline std::vector<FakeI2cWrite> fakeI2cWrites;
inline bool fakeI2cFailBusCreate = false;
inline bool fakeI2cFailTouchRead = false;
inline bool fakeI2cFailRtcRead = false;

inline esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* config, i2c_master_bus_handle_t* out) {
  ++fakeI2cBusCreates;
  fakeI2cBusConfig = *config;
  if (fakeI2cFailBusCreate) {
    *out = nullptr;
    return ESP_FAIL;
  }
  *out = &fakeI2cBus;
  return ESP_OK;
}

inline esp_err_t i2c_master_bus_add_device(const i2c_master_bus_handle_t bus, const i2c_device_config_t* config,
                                           i2c_master_dev_handle_t* out) {
  if (bus != &fakeI2cBus) return ESP_FAIL;
  ++fakeI2cDeviceAdds;
  FakeI2cDevice* device = nullptr;
  if (config->device_address == 0x2E) device = &fakeTouchDevice;
  if (config->device_address == 0x32) device = &fakeRtcDevice;
  if (!device) return ESP_FAIL;
  device->address = static_cast<uint8_t>(config->device_address);
  device->frequency = config->scl_speed_hz;
  *out = device;
  return ESP_OK;
}

inline std::array<uint8_t, 256>& fakeRegisters(const uint8_t address) {
  return address == 0x2E ? fakeTouchRegisters : fakeRtcRegisters;
}

inline esp_err_t i2c_master_transmit(const i2c_master_dev_handle_t device, const uint8_t* data, const size_t length,
                                    int) {
  if (!device || !data || length == 0) return ESP_FAIL;
  fakeI2cWrites.push_back({device->address, std::vector<uint8_t>(data, data + length)});
  auto& registers = fakeRegisters(device->address);
  for (size_t i = 1; i < length; ++i) registers[static_cast<uint8_t>(data[0] + i - 1)] = data[i];
  return ESP_OK;
}

inline esp_err_t i2c_master_transmit_receive(const i2c_master_dev_handle_t device, const uint8_t* writeData,
                                             size_t writeLength, uint8_t* readData, size_t readLength, int) {
  if (!device || !writeData || writeLength != 1 || !readData) return ESP_FAIL;
  if ((device->address == 0x2E && fakeI2cFailTouchRead) ||
      (device->address == 0x32 && fakeI2cFailRtcRead)) return ESP_FAIL;
  const auto& registers = fakeRegisters(device->address);
  for (size_t i = 0; i < readLength; ++i) readData[i] = registers[static_cast<uint8_t>(writeData[0] + i)];
  return ESP_OK;
}