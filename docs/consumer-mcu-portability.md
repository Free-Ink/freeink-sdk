# MCU portability for consumers (C3 ↔ S3)

Every FreeInk SDK library compiles on both ESP32-C3 (X3/X4) and ESP32-S3 (de-link,
M5, Murphy, LilyGo). Pins, geometry, waveforms, SD transport, and orientation come
from `BoardConfig::ACTIVE` or per-driver config, so the SDK hardcodes no chip.

A consumer's own layer can still tie a build to one MCU by hardcoding chip-specific
code. CrossPoint's HAL is written for the C3 (RISC-V): a `pio run -e m5paper`
(ESP32-S3) build fails in `lib/hal/*` — not in any SDK library — at three spots.
This document describes those patterns and their MCU-portable forms.

## Chip-specific patterns and their portable forms

### 1. Deep-sleep GPIO wakeup

`lib/hal/HalGPIO.cpp` and `lib/hal/HalPowerManager.cpp` use the RISC-V deep-sleep
wakeup source:

```cpp
esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN,
                                  ESP_GPIO_WAKEUP_GPIO_LOW);
```

`esp_deep_sleep_enable_gpio_wakeup` / `ESP_GPIO_WAKEUP_GPIO_LOW` is the GPIO
wakeup source on RISC-V parts (C3/C6/H2). Xtensa parts (S3/S2, classic ESP32) wake
from deep sleep through RTC `ext1` (`esp_sleep_enable_ext1_wakeup`), and the wake
pin must be an RTC-capable GPIO. The portable form branches on the SoC capability:

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

The SDK provides this branch in `PowerManager` (below), so a consumer can call that
instead of writing the branch itself.

### 2. Panic backtrace

`lib/hal/HalSystem.cpp`'s `__wrap_panic_print_backtrace` reads a RISC-V exception
frame:

```cpp
uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;  // RISC-V panic_arch.c
```

`RvExcFrame` is the RISC-V exception frame. Xtensa uses `XtExcFrame` with different
fields and windowed-register unwinding, not a flat SP scan. An architecture guard
keeps the custom RISC-V capture and falls back to the default on Xtensa:

```cpp
void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) { __real_panic_print_backtrace(frame, core); return; }
#if __riscv   // or CONFIG_IDF_TARGET_ARCH_RISCV
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  // ... RISC-V capture ...
#else
  __real_panic_print_backtrace(frame, core);  // Xtensa: default backtrace
#endif
}
```

The panic backtrace is an app-level debug hook, so it stays in the consumer behind
this guard rather than in the SDK.

### 3. Hardcoded flash/IO pin

`lib/hal/HalPowerManager.cpp` hardcodes the C3 flash WP pin:

```cpp
constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;  // C3 flash WP pin
```

GPIO13 is the C3's SPI-flash WP line; the S3 uses different flash pins. This pin
belongs behind a target guard or in board config, not as a literal in shared code.

## Pin sourcing: runtime profile, not compile-time default

`InputManager::POWER_BUTTON_PIN` is `constexpr = BoardConfig::DEFAULT_DEVICE.input.power`
— the compile-time default device's pin. For a single-device binary that pin is
correct; for the X3+X4 (and any multi-device) binary the live pin is
`BoardConfig::ACTIVE.input.power`. A consumer's sleep/wake code reads the wake pin
(and any other board pin) from `BoardConfig::ACTIVE`, which the SDK populates per
board.

## `PowerManager`

`libs/hardware/PowerManager` owns the deep-sleep wakeup branch. It selects
`SOC_PM_SUPPORT_EXT1_WAKEUP` vs `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP` at compile time
and reads the wake pin + polarity from `BoardConfig::ACTIVE.input`, so a consumer
writes no chip-specific power code:

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

With `PowerManager` in `lib_deps`, the C3-hardcoded wakeup collapses to:

```cpp
freeink::PowerManager::armPowerButtonWakeup();
esp_deep_sleep_start();
```

`PowerManager.cpp` compiles on both targets — the `gpio` branch links in a C3
build, the `ext1` branch in an S3 build. CrossPoint's `HalGPIO::startDeepSleep`
uses it; `HalPowerManager::startDeepSleep` is the other call site.

## `PowerManager` and the old SDK

`PowerManager` is functionality the upstream `open-x4-sdk` does not have (no
`PowerManager` library, no `freeink` namespace). The rest of freeink-sdk is drop-in
for CrossPoint's existing API through compat shims (`EInkDisplay =
freeink::FreeInkDisplay`, etc.), so repointing the SDK needs no source changes. A
reference to a freeink-only symbol such as `freeink::PowerManager`, though, requires
that the build resolve to freeink-sdk.

CrossPoint's committed `platformio.ini` `[base]` points `lib_deps` at
`open-x4-sdk`; building against freeink-sdk is a local override
(`platformio.local.ini`):

| Build | SDK | `freeink::PowerManager` |
|---|---|---|
| with `platformio.local.ini` | freeink-sdk | compiles |
| without it (CI / release / default) | `open-x4-sdk` | `PowerManager.h` not found |

Of the patterns here, only `PowerManager` is SDK-coupled. The inline `#if SOC_*`
wakeup guard, the `#if __riscv` panic guard, and the flash-pin guard are pure
ESP-IDF and compile against either SDK.

Three ways to handle the coupling:

1. **Inline guard, no SDK dependency** — write the `#if SOC_*` wakeup branch in the
   consumer instead of calling `PowerManager`. Compiles against either SDK; the
   chip branch lives in the consumer.
2. **`__has_include` bridge** — prefer the SDK helper when present, fall back to the
   inline path otherwise, in one snippet that compiles against either SDK:
   ```cpp
   #if __has_include(<PowerManager.h>)
     freeink::PowerManager::armPowerButtonWakeup();
   #elif SOC_PM_SUPPORT_EXT1_WAKEUP
     esp_sleep_enable_ext1_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
   #else
     esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
   #endif
   esp_deep_sleep_start();
   ```
   (The `#include <PowerManager.h>` is guarded with `#if __has_include` too.)
3. **Commit to freeink-sdk** — point the committed `[base]` `lib_deps` (or the
   submodule) at freeink-sdk. The old SDK drops out of the build and plain
   `freeink::PowerManager` resolves. The straight `freeink::PowerManager` form in
   `HalGPIO.cpp` assumes this.

## What a consumer changes

- `HalGPIO.cpp` and `HalPowerManager.cpp` — call `freeink::PowerManager` or the
  `#if SOC_*` wakeup branch instead of `esp_deep_sleep_enable_gpio_wakeup`.
- `HalSystem.cpp` — guard the RISC-V panic backtrace with `#if __riscv`, else call
  `__real_panic_print_backtrace`.
- `HalPowerManager.cpp` — guard or board-source the `GPIO_NUM_13` SPIWP pin.
- Read board pins from `BoardConfig::ACTIVE.*`, not `BoardConfig::DEFAULT_DEVICE.*`
  or fixed constants, in multi-device builds.

These are consumer changes, not SDK changes. With them, one CrossPoint source tree
builds for C3 (X3/X4) and S3 (de-link/M5/Murphy/LilyGo) by device selection.
