#pragma once

// FreeInk SDK — deep-sleep / wake power management.
//
// Owns the one hardware concern the rest of the SDK leaves to the consumer: the
// per-SoC deep-sleep GPIO-wakeup difference. RISC-V parts (C3/C6/H2) wake from
// deep sleep via the "gpio" source (esp_deep_sleep_enable_gpio_wakeup); Xtensa
// parts (S3/S2, classic ESP32) wake via RTC ext1 (esp_sleep_enable_ext1_wakeup).
// Hardcoding either one blocks a multi-MCU build (see docs/consumer-mcu-portability.md).
//
// This picks the right source at compile time from SoC capability macros, and
// reads the wake pin + active level from BoardConfig::ACTIVE.input, so the same
// consumer code deep-sleeps correctly on every supported board. The wake pin must
// be RTC-capable on ext1 parts (true for the de-link power button).

namespace freeink {

class PowerManager {
 public:
  // Arm wake-on-power-button using the SoC-correct wakeup source and the active
  // board's power pin + polarity (powerActiveHigh -> wake on HIGH, else LOW).
  // Returns false if the board has no power pin (PIN_UNASSIGNED); nothing armed.
  static bool armPowerButtonWakeup();

  // Poll the power-button GPIO (raw read, with the matching pull) until released,
  // so deep sleep isn't immediately cancelled by a still-held press.
  static void waitForPowerButtonRelease();

  // Isolate floating GPIOs to cut sleep current, then enter deep sleep. Does not
  // return — the chip resets on wake.
  [[noreturn]] static void deepSleep();

  // Convenience: wait for release, arm the power-button wakeup, then deep sleep.
  [[noreturn]] static void deepSleepUntilPowerButton();
};

}  // namespace freeink
