# Making a consumer MCU-portable (CrossPoint → S3 and beyond)

The FreeInk SDK is MCU-agnostic: every SDK library compiles cleanly on both
ESP32-C3 (X3/X4) and ESP32-S3 (de-link, M5, Murphy). Pins, geometry, waveforms,
SD transport, and orientation all come from `BoardConfig::ACTIVE` or per-driver
config, so the SDK never hardcodes a chip.

A consumer can still block a multi-MCU build by hardcoding chip-specific code in
**its own** layer. CrossPoint does today: a `pio run -e m5paper` (ESP32-S3) build
fails in `lib/hal/*` — **not** in any SDK library — with three errors, all because
the HAL is written for the C3 (RISC-V) and assumes C3-only APIs. This document is
what CrossPoint needs to change to be SDK-compatible across MCUs.

> Reproduce: `pio run -e m5paper` from the CrossPoint repo. Every FreeInk library
> compiles; the build dies in `lib/hal/HalGPIO.cpp`, `HalPowerManager.cpp`, and
> `HalSystem.cpp`.

---

## The three blockers

### 1. Deep-sleep GPIO wakeup uses the C3-only API

`lib/hal/HalGPIO.cpp:235` and `lib/hal/HalPowerManager.cpp:92`:

```cpp
esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN,
                                  ESP_GPIO_WAKEUP_GPIO_LOW);
```

`esp_deep_sleep_enable_gpio_wakeup` / `ESP_GPIO_WAKEUP_GPIO_LOW` is the
**RISC-V chips' "GPIO" deep-sleep wakeup source** (C3/C6/H2). The S3 (and S2,
classic ESP32) wake from deep sleep through **RTC IO `ext1`** instead, which the
compiler even suggests (`ESP_EXT1_WAKEUP_ANY_LOW`). The wake pin must be an
RTC-capable GPIO on those parts.

**Fix — branch on the SoC capability:**

```cpp
#include <esp_sleep.h>
#include <soc/soc_caps.h>

void enablePowerButtonWakeup(int pin) {
  const uint64_t mask = 1ULL << pin;
#if SOC_PM_SUPPORT_EXT1_WAKEUP            // S3 / S2 / classic ESP32 (Xtensa)
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP  // C3 / C6 / H2 (RISC-V)
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
#else
#error "No deep-sleep GPIO wakeup path for this target"
#endif
}
```

### 2. The panic backtrace assumes a RISC-V CPU

`lib/hal/HalSystem.cpp:46`, inside `__wrap_panic_print_backtrace`:

```cpp
// Copied from components/esp_system/port/arch/riscv/panic_arch.c
uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
```

`RvExcFrame` is the **RISC-V** exception frame. The S3 is **Xtensa**, whose frame
type is `XtExcFrame` with different fields, and whose stack unwinding is not the
flat SP scan this code does (Xtensa uses windowed registers). This whole custom
backtrace is C3-specific.

**Fix — guard the custom path by architecture and fall back otherwise:**

```cpp
void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) { __real_panic_print_backtrace(frame, core); return; }
#if __riscv   // (or #if CONFIG_IDF_TARGET_ARCH_RISCV)
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  ...                                   // existing RISC-V capture
#else
  __real_panic_print_backtrace(frame, core);  // Xtensa: defer to the default
#endif
}
```

A native Xtensa capture can come later; the fallback keeps S3 building and still
prints a usable backtrace.

### 3. Hardcoded flash/IO pin in the sleep path

`lib/hal/HalPowerManager.cpp:81`:

```cpp
constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;  // C3 flash WP pin
```

GPIO13 is the C3's SPI-flash WP line; the S3 uses different flash pins. Either
guard this per target or source it from board config — it should not be a literal
in shared code.

---

## Pin sourcing: use the *runtime* profile, not the compile-time default

`InputManager::POWER_BUTTON_PIN` is `constexpr = BoardConfig::DEFAULT_DEVICE.input.power`
— the **compile-time default device's** pin. That's correct for a single-device
binary, but for the X3+X4 (and any future multi-device) build the live pin is
`BoardConfig::ACTIVE.input.power`. CrossPoint's sleep/wake code should read the
wake pin (and any other board pin) from `BoardConfig::ACTIVE`, which the SDK
already populates per board. Same rule for the SPIWP/flash pin above.

---

## The SDK now owns the wakeup portability (`PowerManager`)

Issue #1 is the SDK's job — it already knows the active board — so the SDK now
ships `libs/hardware/PowerManager`. It picks the `SOC_PM_SUPPORT_EXT1_WAKEUP` vs
`SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP` branch at compile time and reads the wake pin
+ polarity from `BoardConfig::ACTIVE.input`, so consumers write no chip-specific
power code:

```cpp
namespace freeink {
class PowerManager {
  static bool armPowerButtonWakeup();        // SoC-correct source + active pin/polarity
  static void waitForPowerButtonRelease();   // raw GPIO poll until released
  [[noreturn]] static void deepSleep();      // isolate GPIOs, then deep sleep
  [[noreturn]] static void deepSleepUntilPowerButton();  // all three, in order
};
}
```

Add `PowerManager` to `lib_deps` and the C3-hardcoded block collapses. CrossPoint's
`HalGPIO::startDeepSleep` already adopts it:

```cpp
// before — C3-only, breaks on S3:
esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
esp_deep_sleep_start();

// after — MCU-portable:
freeink::PowerManager::armPowerButtonWakeup();
esp_deep_sleep_start();
```

`PowerManager.cpp` is verified to compile on **both** targets (the `gpio` branch
links in the C3 build, the `ext1` branch compiles in the S3 build). The second
call site (`HalPowerManager::startDeepSleep`) still needs the same one-line swap —
that's the remaining issue-#1 adoption. The panic backtrace (issue #2) is an
app-level debug hook, so it stays in CrossPoint behind an arch guard.

---

## Checklist for CrossPoint

- [x] `HalGPIO.cpp` — now calls `freeink::PowerManager::armPowerButtonWakeup()`.
- [ ] `HalPowerManager.cpp:92` — same one-line swap to `PowerManager` (still C3-only).
- [ ] Guard the RISC-V panic backtrace with `#if __riscv`, else fall back to
      `__real_panic_print_backtrace` — `HalSystem.cpp`.
- [ ] Guard or board-source the `GPIO_NUM_13` SPIWP pin — `HalPowerManager.cpp`.
- [ ] Read board pins from `BoardConfig::ACTIVE.*`, not
      `BoardConfig::DEFAULT_DEVICE.*` / fixed constants, in multi-device builds.
- [ ] `pio run -e m5paper` compiles (proves the HAL is MCU-portable; the M5 panel
      driver itself is a separate stub concern).

None of these are SDK changes — they are CrossPoint making its own HAL as
MCU-neutral as the SDK already is. Once done, the same CrossPoint source builds
for C3 (X3/X4) and S3 (de-link/M5/Murphy) by selecting the device, exactly as the
SDK intends.
