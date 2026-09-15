#include <cassert>
#include <cstdio>
#include <climits>
#include <HapticManager.h>
#include <BoardConfig.h>
#include <Arduino.h>
#include <InputManager.h>
#include <algorithm>
#if FREEINK_CAP_HAPTIC
#include <esp_timer.h>
#include <freertos/semphr.h>
#endif

int main(int argc, char**) {
  auto& haptic = HapticManager::getInstance();
#if FREEINK_CAP_HAPTIC
  if (argc > 1) {
    failMutex = true;
    assert(!haptic.begin());
    assert(!haptic.pulse());
    haptic.stop(); haptic.end(); haptic.setEnabled(false);
    assert(!haptic.present() && !haptic.isPlaying());
    std::puts("Haptic mutex allocation failure passed");
    return 0;
  }
  static_assert(BoardConfig::METALIO_EINK4.haptic.gpio == 44);
  assert(haptic.supported() && !haptic.present());
  assert(!haptic.pulse()); // explicit init
  failCreate = true;
  assert(!haptic.begin() && gpioLevels[44] == LOW);
  failCreate = false;
  failAttach = true;
  assert(!haptic.begin());
  failAttach = false;
  assert(haptic.begin());
  assert(pwmAttached && pwmPin == 44 && pwmFrequency == 20000 && pwmDuty == 0);

  // Consumer policy: neither screen touches nor cover keys start a pulse.
  std::fill(std::begin(gpioLevels), std::end(gpioLevels), HIGH);
  InputManager input;
  input.begin();
  const auto beforeTouch = pwmWrites;
  for (const auto y : {100, 900}) {
    Wire.touch = {1, 0, 80, uint8_t(y >> 8), uint8_t(y)};
    touchIsr(); fakeNow += 25; input.update();
    Wire.touch[0] = 0;
    touchIsr(); fakeNow += 25; input.update();
  }
  assert(pwmWrites == beforeTouch && pwmDuty == 0 && !haptic.isPlaying());

  const auto start = fakeNow;
  assert(haptic.pulse());
  assert(fakeNow == start && pwmDuty == 255 && haptic.isPlaying());
  advance(34); assert(pwmDuty == 255);
  advance(1); assert(pwmDuty == 0 && !haptic.isPlaying());

  haptic.setIntensity(128);
  assert(haptic.pulse(50, 128) && pwmDuty == 64);
  haptic.setIntensity(255); assert(pwmDuty == 128);
  haptic.setIntensity(0); assert(pwmDuty == 0 && haptic.isPlaying());
  haptic.stop();
  haptic.setIntensity(255);

  HapticManager::Step pattern[] = {{20, 100}, {30, 0}, {40, 200}};
  assert(haptic.play(pattern, 3, 2));
  pattern[0].intensity = 1; // playback owns its copy
  advance(20); assert(pwmDuty == 0 && haptic.isPlaying());
  advance(30); assert(pwmDuty == 200);
  advance(40); assert(pwmDuty == 100); // repeat uses original copied value
  advance(90); assert(pwmDuty == 0 && !haptic.isPlaying());

  assert(haptic.pulse(100, 50));
  advance(10);
  assert(haptic.pulse(60, 200));
  const auto replacementDue = fakeTimer.due;
  fakeTimer.callback(fakeTimer.arg); // queued callback from replaced pattern
  assert(pwmDuty == 200 && fakeTimer.due == replacementDue);
  advance(60); assert(pwmDuty == 0);

  const HapticManager::Step delayed[] = {{10, 255}, {10, 0}, {100, 128}};
  assert(haptic.play(delayed, 3, 2));
  advance(150); assert(pwmDuty == 128); // skip expired phases, including a repeat
  advance(100); assert(!haptic.isPlaying() && pwmDuty == 0);

  assert(haptic.pulse(100));
  const auto due = fakeTimer.due;
  const HapticManager::Step invalid{0, 255};
  assert(!haptic.play(nullptr, 1));
  assert(!haptic.play(&invalid, 1));
  assert(!haptic.play(delayed, 0));
  assert(!haptic.play(delayed, 33));
  assert(!haptic.play(delayed, 3, 0));
  assert(!haptic.pulse(0));
  assert(pwmDuty == 255 && fakeTimer.due == due);
  haptic.setEnabled(false);
  assert(!haptic.isPlaying() && pwmDuty == 0 && !haptic.pulse());
  haptic.setEnabled(true);
  assert(!haptic.isPlaying());

  failStart = true;
  assert(!haptic.pulse() && !haptic.isPlaying() && pwmDuty == 0);
  failStart = false;
  assert(haptic.play(delayed, 3));
  failStart = true;
  advance(10); // failure when transitioning to the next step
  assert(!haptic.isPlaying() && pwmDuty == 0);
  failStart = false;
  failWrite = true;
  assert(!haptic.pulse() && !haptic.present() && !pwmAttached && gpioLevels[44] == LOW);
  failWrite = false;
  assert(haptic.begin());

  // Large durations use 64-bit microsecond arithmetic.
  const HapticManager::Step large{UINT32_MAX, 255};
  assert(haptic.play(&large, 1, 65535));
  assert(fakeTimer.due - esp_timer_get_time() == uint64_t(UINT32_MAX) * 1000);
  haptic.end();
  assert(!pwmAttached && gpioLevels[44] == LOW && !haptic.present());
  fakeTimer.callback(fakeTimer.arg); // end prevents queued callbacks from driving pin
  assert(!pwmAttached && gpioLevels[44] == LOW);

  BoardConfig::ACTIVE.haptic = {-1, true, 20000};
  assert(!haptic.supported() && !haptic.begin());
  BoardConfig::ACTIVE.haptic = {44, false, 20000};
  assert(haptic.begin() && pwmDuty == 255);
  assert(haptic.pulse(10, 128) && pwmDuty == 127);
  advance(10); assert(pwmDuty == 255);
  haptic.end(); assert(gpioLevels[44] == HIGH);
  std::puts("Haptic pulses, intensity, copied patterns, repeats, replacement, timing, failures and lifecycle passed");
#else
  assert(!haptic.supported() && !haptic.begin() && !haptic.present());
  assert(!haptic.pulse() && !haptic.play(nullptr, 1));
  haptic.setEnabled(false); assert(!haptic.enabled());
  haptic.setIntensity(100); assert(haptic.intensity() == 100);
  haptic.stop(); haptic.end();
  assert(!haptic.isPlaying() && !pwmAttached && pwmWrites == 0);
  std::puts("Disabled haptic backend passed without timer or PWM dependencies");
#endif
}
