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

## Recommended: push the portable bits into the SDK

The cleanest end state is that consumers never write chip-specific power code at
all. The SDK already owns display/input/battery/storage/orientation/SD-transport;
**deep sleep + wake is the remaining hardware concern it doesn't yet abstract.**
Adding a small SDK power helper would let CrossPoint delete its per-MCU `#ifdef`s:

```cpp
// proposed SDK API (libs/hardware/PowerManager or similar)
namespace freeink {
// Arms wake-on-power-button (from BoardConfig::ACTIVE.input.power, honoring
// powerActiveHigh) using the correct per-SoC wakeup source, then deep-sleeps.
void deepSleepUntilPowerButton();
}
```

The SDK is the right home for the `SOC_PM_SUPPORT_EXT1_WAKEUP` vs
`SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP` branch (issue #1) and the wake-pin lookup,
because it already knows the active board. The panic backtrace (issue #2) is an
app-level debug hook, so it can stay in CrossPoint behind an arch guard.

---

## Checklist for CrossPoint

- [ ] Replace `esp_deep_sleep_enable_gpio_wakeup` with a `SOC_*`-guarded wakeup
      (or call an SDK `deepSleepUntilPowerButton()` helper) — `HalGPIO.cpp`,
      `HalPowerManager.cpp`.
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
