#pragma once

#include <esp_mac.h>
#include <stdint.h>

namespace freeink {

// Stock EEGO UID: final three bytes of the factory MAC, independent of Wi-Fi state.
// Leave the output unchanged if the factory identity cannot be read.
inline bool readEegoA4Uid(uint32_t& uid) {
  uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return false;
  uid = static_cast<uint32_t>(mac[3]) << 16 | static_cast<uint32_t>(mac[4]) << 8 | mac[5];
  return true;
}

}  // namespace freeink
