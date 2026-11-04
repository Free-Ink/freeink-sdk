#pragma once

#include <BatteryMonitor.h>

#include <algorithm>
#include <cstdint>

// Allocation-free Picco LiHV presentation filter. The caller supplies elapsed
// time, including sleep, and owns sampling/retention. No timers, I/O, or charger
// configuration here; the voltage guard points come from Picco's discharge curve.
class BatteryEstimate {
 public:
  enum class Power : uint8_t { Unknown, Battery, Charging, External, Full };
  struct State {
    int32_t percentQ8;
    uint32_t settlingMs;
    Power power;
  };

  static constexpr uint32_t SETTLING_MS = 120000;
  static constexpr uint32_t FILTER_MS = 180000;
  static constexpr uint32_t REBASE_MS = 1800000;

  bool known() const { return state_.percentQ8 >= 0; }
  uint16_t percentage() const { return known() ? static_cast<uint16_t>((state_.percentQ8 + 128) / 256) : 0; }
  const State& state() const { return state_; }
  static bool valid(const State& state) {
    return state.percentQ8 >= 0 && state.percentQ8 <= 100 * 256 && state.settlingMs <= SETTLING_MS &&
           static_cast<uint8_t>(state.power) <= static_cast<uint8_t>(Power::Full);
  }
  bool restore(const State& state) {
    if (!valid(state)) return false;
    state_ = state;
    return true;
  }

  uint16_t update(const BatteryMonitor::Status& sample, uint32_t elapsedMs) {
    if (!sample.percentageKnown || !sample.millivoltsKnown || sample.percentage > 100 || sample.millivolts < 2500 ||
        sample.millivolts > 4500)
      return percentage();

    Power power = Power::Unknown;
    if (sample.externalPowerKnown && sample.chargingKnown) {
      if (!sample.externalPower)
        power = Power::Battery;
      else if (sample.chargeCompleteKnown && sample.chargeComplete && sample.millivolts >= 4229)
        power = Power::Full;  // Require a plausible near-full voltage as well as charger completion.
      else
        power = sample.charging ? Power::Charging : Power::External;
    }
    // The stock voltage estimate can reach 100% while charging continues.
    // Power::Full still specifically means the charger reported completion;
    // an unread charger cannot justify selecting the high charging estimate.
    const uint16_t voltagePercent =
        power == Power::Unknown ? std::min<uint16_t>(sample.percentage, 99) : sample.percentage;
    const int32_t target = (power == Power::Full ? 100 : voltagePercent) * 256;
    const bool rebase = !known() || elapsedMs >= REBASE_MS;
    // Low voltage must not wait for a display filter or USB settling.
    const bool low = sample.millivolts <= 3636 && sample.percentage <= 10;
    // Even after a long sleep, an unread charger cannot establish which curve
    // applies. Preserve history until status recovers, except at low voltage.
    if (known() && power == Power::Unknown && !low) return percentage();
    if (rebase || power == Power::Full || (low && target < state_.percentQ8)) {
      state_ = {target, 0, power};
      return percentage();
    }
    if (power != state_.power) {
      state_.power = power;
      state_.settlingMs = SETTLING_MS;
      return percentage();
    }
    if (state_.settlingMs > 0) {
      const uint32_t heldMs = std::min(elapsedMs, state_.settlingMs);
      state_.settlingMs -= heldMs;
      elapsedMs -= heldMs;
    }
    // Fixed-point arithmetic avoids the old repeated downward integer rounding.
    const int32_t delta = target - state_.percentQ8;
    const uint32_t denominator = FILTER_MS + elapsedMs;
    const int32_t step = static_cast<int32_t>(
        (static_cast<uint64_t>(delta < 0 ? -delta : delta) * elapsedMs + denominator / 2) / denominator);
    state_.percentQ8 += delta < 0 ? -step : step;
    return percentage();
  }

 private:
  State state_{-1, 0, Power::Unknown};
};
