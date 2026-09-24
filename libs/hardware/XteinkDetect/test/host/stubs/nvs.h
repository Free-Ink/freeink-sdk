#pragma once
#include <cstdint>
using nvs_handle_t = int;
using esp_err_t = int;
constexpr int NVS_READONLY=0, ESP_OK=0;
inline int nvs_open(const char*,int,nvs_handle_t*) { return -1; }
inline int nvs_get_u8(nvs_handle_t,const char*,uint8_t*) { return -1; }
inline void nvs_close(nvs_handle_t) {}
