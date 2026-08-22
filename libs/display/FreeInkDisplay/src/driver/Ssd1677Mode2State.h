#pragma once

#include <cstdint>

namespace freeink {

// Pure bookkeeping for the opt-in SSD1677 RAM ping-pong experiment. Hardware
// writes stay in Ssd1677Driver; keeping the transition model independent makes
// odd/even, window/full, clean, and reset behavior host-testable.
class Ssd1677Mode2State {
 public:
  enum class Phase : uint8_t { NeedsSeed, Seeded, Active, NeedsSync };
  enum class Plan : uint8_t { Legacy, SeedAbsolute, EnableAndUpdate, PingPongUpdate, ResetAndSeedAbsolute };

  constexpr Plan plan(const bool compatibleFast) const {
    if (!_available) return Plan::Legacy;
    if (_phase == Phase::NeedsSync) return Plan::ResetAndSeedAbsolute;
    if (!compatibleFast) return _phase == Phase::Active ? Plan::ResetAndSeedAbsolute : Plan::Legacy;
    if (_phase == Phase::NeedsSeed) return Plan::SeedAbsolute;
    if (_phase == Phase::Seeded) return Plan::EnableAndUpdate;
    return Plan::PingPongUpdate;
  }

  constexpr void setAvailable(const bool available) {
    _available = available;
    controllerReset();
  }

  constexpr void controllerReset() {
    _phase = Phase::NeedsSeed;
    _activationCount = 0;
    _bankParity = 0;
  }

  constexpr void invalidate() { controllerReset(); }

  constexpr void seeded() {
    if (_available) {
      _phase = Phase::Seeded;
      _activationCount = 0;
      _bankParity = 0;
    }
  }

  constexpr void enabled() {
    if (_available && _phase == Phase::Seeded) _phase = Phase::Active;
  }

  constexpr void presented() {
    if (_phase != Phase::Active) return;
    ++_activationCount;
    _bankParity ^= 1;
    _phase = Phase::NeedsSync;
  }

  constexpr void resynchronized() {
    if (_phase == Phase::NeedsSync) _phase = Phase::Active;
  }

  constexpr bool available() const { return _available; }
  constexpr bool active() const { return _phase == Phase::Active || _phase == Phase::NeedsSync; }
  constexpr Phase phase() const { return _phase; }
  constexpr uint32_t activationCount() const { return _activationCount; }
  constexpr uint8_t bankParity() const { return _bankParity; }

 private:
  bool _available = false;
  Phase _phase = Phase::NeedsSeed;
  uint32_t _activationCount = 0;
  uint8_t _bankParity = 0;
};

}  // namespace freeink
