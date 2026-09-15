#include "HapticManager.h"

#include <BoardConfig.h>

#if FREEINK_CAP_HAPTIC
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace freeink {
namespace {
struct State {
  SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  esp_timer_handle_t timer = nullptr;
  BoardConfig::HapticConfig config{};
  HapticManager::Step steps[HapticManager::kMaxPatternSteps]{};
  size_t count = 0;
  uint16_t repeats = 0;
  uint64_t patternUs = 0;
  int64_t startedAt = 0;
  bool begun = false;
  bool playing = false;
  bool enabled = true;
  uint8_t gain = 255;
  uint8_t stepIntensity = 0;
};

// Singleton lifetime: timer callbacks never refer to a destroyed consumer object.
// Keep the timer/mutex across end()/begin(); end() releases the PWM channel.
State& state() {
  static State s;
  return s;
}

class Lock {
 public:
  explicit Lock(State& s) : mutex_(s.mutex) {
    locked_ = mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }
  ~Lock() { if (locked_) xSemaphoreGive(mutex_); }
  explicit operator bool() const { return locked_; }
 private:
  SemaphoreHandle_t mutex_;
  bool locked_;
};

uint32_t duty(const State& s, uint8_t level) {
  const uint32_t scaled = (uint32_t(level) * s.gain + 127) / 255;
  return s.config.activeHigh ? scaled : 255 - scaled;
}

void off(State& s) {
  if (!s.begun) return;
  if (!ledcWrite(s.config.gpio, s.config.activeHigh ? 0 : 255)) {
    // If PWM ownership was lost, leave a plain inactive GPIO instead.
    ledcDetach(s.config.gpio);
    pinMode(s.config.gpio, OUTPUT);
    digitalWrite(s.config.gpio, s.config.activeHigh ? LOW : HIGH);
    s.begun = false;
  }
}

void stopLocked(State& s) {
  s.playing = false;
  s.stepIntensity = 0;
  if (s.timer) esp_timer_stop(s.timer);
  off(s);
}

// Resolve against elapsed time rather than replaying expired steps if the timer
// task runs late. A queued callback from an older request sees the new timeline,
// so it cannot turn off or advance a replacement pattern prematurely.
bool serviceLocked(State& s) {
  if (!s.playing) return false;
  const uint64_t elapsed = static_cast<uint64_t>(esp_timer_get_time() - s.startedAt);
  if (elapsed >= s.patternUs * s.repeats) {
    stopLocked(s);
    return true;
  }
  const uint64_t position = elapsed % s.patternUs;
  uint64_t boundary = 0;
  for (size_t i = 0; i < s.count; ++i) {
    boundary += uint64_t(s.steps[i].durationMs) * 1000;
    if (position >= boundary) continue;
    s.stepIntensity = s.steps[i].intensity;
    esp_timer_stop(s.timer);
    if (esp_timer_start_once(s.timer, boundary - position) != ESP_OK ||
        !ledcWrite(s.config.gpio, duty(s, s.stepIntensity))) {
      stopLocked(s);
      return false;
    }
    return true;
  }
  stopLocked(s);
  return false;
}

void timerCallback(void*) {
  auto& s = state();
  Lock lock(s);
  if (lock) serviceLocked(s);
}
}  // namespace

HapticManager& HapticManager::getInstance() {
  static HapticManager manager;
  return manager;
}

bool HapticManager::supported() const {
  return BoardConfig::ACTIVE.haptic.gpio >= 0 && BoardConfig::ACTIVE.haptic.pwmFrequency > 0;
}

bool HapticManager::begin() {
  auto& s = state();
  Lock lock(s);
  if (!lock) return false;
  if (s.begun) return true;
  if (!supported()) return false;
#if FREEINK_DEVICE_METALIO_EINK4
  // Finish board initialization before attaching PWM: the legacy board startup
  // configures the motor pad LOW. Later managers must not reset our PWM pin.
  if (BoardConfig::isMetalioEInk4() && !metalio::ensureBooted()) return false;
#endif
  s.config = BoardConfig::ACTIVE.haptic;
  pinMode(s.config.gpio, OUTPUT);
  digitalWrite(s.config.gpio, s.config.activeHigh ? LOW : HIGH);
  if (!s.timer) {
    esp_timer_create_args_t args{};
    args.callback = timerCallback;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "freeink_haptic";
    if (esp_timer_create(&args, &s.timer) != ESP_OK) return false;
  }
  if (!ledcAttach(s.config.gpio, s.config.pwmFrequency, 8)) return false;
  s.begun = true;
  off(s);
  return s.begun;
}

bool HapticManager::present() const {
  auto& s = state(); Lock lock(s);
  return lock && s.begun;
}

void HapticManager::end() {
  auto& s = state(); Lock lock(s);
  if (!lock) return;
  stopLocked(s);
  if (!s.begun) return;
  ledcDetach(s.config.gpio);
  pinMode(s.config.gpio, OUTPUT);
  digitalWrite(s.config.gpio, s.config.activeHigh ? LOW : HIGH);
  s.begun = false;
}

bool HapticManager::pulse(uint32_t durationMs, uint8_t level) {
  const Step step{durationMs, level};
  return play(&step, 1);
}

bool HapticManager::play(const Step* steps, size_t count, uint16_t repeats) {
  if (!steps || count == 0 || count > kMaxPatternSteps || repeats == 0) return false;
  uint64_t duration = 0;
  for (size_t i = 0; i < count; ++i) {
    if (steps[i].durationMs == 0) return false;
    duration += uint64_t(steps[i].durationMs) * 1000;
  }
  auto& s = state(); Lock lock(s);
  if (!lock || !s.begun || !s.enabled) return false;
  stopLocked(s);
  if (!s.begun) return false;
  for (size_t i = 0; i < count; ++i) s.steps[i] = steps[i];
  s.count = count;
  s.repeats = repeats;
  s.patternUs = duration;
  s.startedAt = esp_timer_get_time();
  s.playing = true;
  return serviceLocked(s);
}

void HapticManager::stop() {
  auto& s = state(); Lock lock(s);
  if (lock) stopLocked(s);
}

bool HapticManager::isPlaying() const {
  auto& s = state(); Lock lock(s);
  return lock && s.playing;
}

void HapticManager::setEnabled(bool enabled) {
  auto& s = state(); Lock lock(s);
  if (!lock) return;
  s.enabled = enabled;
  if (!enabled) stopLocked(s);
}

bool HapticManager::enabled() const {
  auto& s = state(); Lock lock(s);
  return lock && s.enabled;
}

void HapticManager::setIntensity(uint8_t gain) {
  auto& s = state(); Lock lock(s);
  if (!lock) return;
  s.gain = gain;
  if (s.playing) serviceLocked(s);
}

uint8_t HapticManager::intensity() const {
  auto& s = state(); Lock lock(s);
  return lock ? s.gain : 0;
}
}  // namespace freeink

#else
#include <atomic>
namespace freeink {
namespace { std::atomic<bool> userEnabled{true}; std::atomic<uint8_t> userGain{255}; }
HapticManager& HapticManager::getInstance() { static HapticManager manager; return manager; }
bool HapticManager::supported() const { return false; }
bool HapticManager::begin() { return false; }
bool HapticManager::present() const { return false; }
void HapticManager::end() {}
bool HapticManager::pulse(uint32_t, uint8_t) { return false; }
bool HapticManager::play(const Step*, size_t, uint16_t) { return false; }
void HapticManager::stop() {}
bool HapticManager::isPlaying() const { return false; }
void HapticManager::setEnabled(bool enabled) { userEnabled = enabled; }
bool HapticManager::enabled() const { return userEnabled; }
void HapticManager::setIntensity(uint8_t gain) { userGain = gain; }
uint8_t HapticManager::intensity() const { return userGain; }
}  // namespace freeink
#endif
