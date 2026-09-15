#pragma once

#include <cstddef>
#include <cstdint>

namespace freeink {

// One owner for the board's vibration motor. All calls are task-context only.
// Playback uses a timer task: no update() loop and no delay() in pulse/play.
// InputManager and UI code never trigger this manager automatically.
class HapticManager {
 public:
  struct Step {
    uint32_t durationMs;
    uint8_t intensity;  // 0 = pause, 255 = full drive (before master scaling)
  };
  static constexpr size_t kMaxPatternSteps = 32;
  static constexpr uint32_t kDefaultPulseMs = 35;

  static HapticManager& getInstance();
  HapticManager(const HapticManager&) = delete;
  HapticManager& operator=(const HapticManager&) = delete;

  bool supported() const;  // capability compiled in and active board has a motor
  bool begin();           // attach output, initially silent; timer/PWM errors are retryable
  bool present() const;   // successfully initialized
  void end();             // stop and release PWM; begin() can attach again

  // Requires begin() and enabled(). A successful request replaces playback.
  // Patterns are copied, so stack arrays are safe. Invalid requests (null/empty,
  // >32 steps, zero-duration step, zero repeats) leave current playback intact.
  bool pulse(uint32_t durationMs = kDefaultPulseMs, uint8_t intensity = 255);
  bool play(const Step* steps, size_t count, uint16_t repeats = 1);
  void stop();
  bool isPlaying() const;  // true during pauses too

  void setEnabled(bool enabled);  // disabling stops; enabling does not resume
  bool enabled() const;
  void setIntensity(uint8_t intensity);  // master gain 0..255, applies immediately
  uint8_t intensity() const;

 private:
  HapticManager() = default;
};

}  // namespace freeink

using HapticManager = freeink::HapticManager;
