#pragma once

#include "sdkconfig.h"

// 本 fork 只保留 ESP32-S3 的 LCD/CAM 外设输出路径。
#ifndef CONFIG_IDF_TARGET_ESP32S3
#error "this epdiy fork only supports ESP32-S3 (LCD render path)"
#endif
#define RENDER_METHOD_LCD 1

#ifdef __clang__
#define IRAM_ATTR
// define this if we're using clangd to make it accept the GCC builtin
void __assert_func(const char* file, int line, const char* func, const char* failedexpr);
#endif
