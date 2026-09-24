#pragma once

// Self-contained logging for the OPDS module, mirroring the host app's LOG_*
// signatures so call sites move unchanged. On host builds (ENABLE_SERIAL_LOG
// undefined) the macros expand to nothing and pull in no Arduino headers.
#ifdef ENABLE_SERIAL_LOG
#include <Arduino.h>
#define LOG_ERR(origin, format, ...) Serial.printf("[%s] " format "\n", origin, ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) Serial.printf("[%s] " format "\n", origin, ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...) Serial.printf("[%s] " format "\n", origin, ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...)
#define LOG_INF(origin, format, ...)
#define LOG_DBG(origin, format, ...)
#endif
