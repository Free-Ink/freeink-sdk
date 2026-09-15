#pragma once
#include <cstdint>
#include <Arduino.h>
constexpr int ESP_OK = 0, ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
  void (*callback)(void*);
  void* arg;
  int dispatch_method;
  const char* name;
};
struct FakeTimer {
  void (*callback)(void*) = nullptr;
  void* arg = nullptr;
  uint64_t due = 0;
  bool armed = false;
};
using esp_timer_handle_t = FakeTimer*;
inline FakeTimer fakeTimer;
inline bool failCreate = false, failStart = false;
inline int64_t esp_timer_get_time() { return int64_t(fakeNow) * 1000; }
inline int esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* timer) {
  if (failCreate) return -1;
  fakeTimer.callback = args->callback; fakeTimer.arg = args->arg;
  *timer = &fakeTimer;
  return ESP_OK;
}
inline int esp_timer_stop(esp_timer_handle_t timer) { timer->armed = false; return ESP_OK; }
inline int esp_timer_start_once(esp_timer_handle_t timer, uint64_t us) {
  if (failStart) return -1;
  timer->due = esp_timer_get_time() + us; timer->armed = true;
  return ESP_OK;
}
inline void advance(unsigned ms) {
  fakeNow += ms;
  if (fakeTimer.armed && uint64_t(esp_timer_get_time()) >= fakeTimer.due) {
    fakeTimer.armed = false;
    fakeTimer.callback(fakeTimer.arg);
  }
}
