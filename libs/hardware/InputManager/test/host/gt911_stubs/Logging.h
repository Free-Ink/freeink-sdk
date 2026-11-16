#pragma once
// Counts the firmware log calls so the test can check that every INT-wake
// failure is reported, and reported once.
#include <cstdio>

inline unsigned loggedErrors = 0;
inline unsigned loggedInfos = 0;
inline char lastLog[256] = {};
#define LOG_ERR(origin, ...) (++loggedErrors, std::snprintf(lastLog, sizeof(lastLog), __VA_ARGS__))
#define LOG_INF(origin, ...) (++loggedInfos, std::snprintf(lastLog, sizeof(lastLog), __VA_ARGS__))
