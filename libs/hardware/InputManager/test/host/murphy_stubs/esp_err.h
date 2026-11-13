#pragma once

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
inline const char* esp_err_to_name(const esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }