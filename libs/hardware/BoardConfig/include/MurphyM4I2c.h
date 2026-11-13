#pragma once

#include <BoardConfig.h>

#if FREEINK_DEVICE_MURPHY_M4

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_rom_sys.h>

namespace freeink::murphy_m4_i2c {

// The IDF owns the small handle allocations. They are created once and kept
// until reset so touch/RTC traffic never allocates in the polling path.
inline i2c_master_bus_handle_t bus = nullptr;
inline i2c_master_dev_handle_t touch = nullptr;
inline i2c_master_dev_handle_t rtc = nullptr;
inline bool busAttempted = false;
inline bool touchAttempted = false;
inline bool rtcAttempted = false;

inline bool beginBus(const int sda, const int scl) {
  if (busAttempted)
    return bus != nullptr;
  busAttempted = true;

  i2c_master_bus_config_t config = {};
  config.i2c_port = I2C_NUM_1;
  config.sda_io_num = static_cast<gpio_num_t>(sda);
  config.scl_io_num = static_cast<gpio_num_t>(scl);
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = 1;
  const esp_err_t err = i2c_new_master_bus(&config, &bus);
  if (err != ESP_OK) {
    esp_rom_printf("[i2c] M4 I2C1 init failed: %s (%d)\r\n",
                   esp_err_to_name(err), static_cast<int>(err));
    bus = nullptr;
  }
  return bus != nullptr;
}

inline i2c_master_dev_handle_t addDevice(i2c_master_dev_handle_t &handle,
                                         bool &attempted, const char *name,
                                         const int sda, const int scl,
                                         const uint8_t address,
                                         const uint32_t frequency) {
  if (attempted)
    return handle;
  attempted = true;
  if (!beginBus(sda, scl))
    return nullptr;

  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = frequency;
  const esp_err_t err = i2c_master_bus_add_device(bus, &config, &handle);
  if (err != ESP_OK) {
    esp_rom_printf("[i2c] M4 %s device init failed: %s (%d)\r\n", name,
                   esp_err_to_name(err), static_cast<int>(err));
    handle = nullptr;
  }
  return handle;
}

inline i2c_master_dev_handle_t touchDevice(const int sda, const int scl,
                                           const uint8_t address) {
  return addDevice(touch, touchAttempted, "touch", sda, scl, address, 100000);
}

inline i2c_master_dev_handle_t rtcDevice(const int sda, const int scl,
                                         const uint8_t address) {
  return addDevice(rtc, rtcAttempted, "RTC", sda, scl, address, 400000);
}

inline bool write(i2c_master_dev_handle_t device, const uint8_t *data,
                  const size_t length) {
  return device != nullptr &&
         i2c_master_transmit(device, data, length, 20) == ESP_OK;
}

inline bool read(i2c_master_dev_handle_t device, const uint8_t reg,
                 uint8_t *data, const size_t length) {
  return device != nullptr && i2c_master_transmit_receive(device, &reg, 1, data,
                                                          length, 20) == ESP_OK;
}

} // namespace freeink::murphy_m4_i2c

#endif
