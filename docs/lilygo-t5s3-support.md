# LilyGo T5 S3 (4.7" ED047TC1) — support status & bring-up

The LilyGo T5S3-4.7-ePaper (PRO/Lite) is an ESP32-S3 board whose panel is an
**ED047TC1: a raw 960×540 16-gray parallel EPD with no on-glass controller**. The
MCU clocks every row over the S3 LCD (i80) peripheral and an external PMIC
generates the waveform rails — a different display class from FreeInk's SPI
single-chip drivers (SSD1677/UC8253/ED2208).

Reference port: [ShallowGreen123/t5s3-reader](https://github.com/ShallowGreen123/t5s3-reader).

## What the SDK provides

- **`LgfxEpdDriver`** — wraps **LovyanGFX's `Panel_EPD`/`Bus_EPD`** (bundled in
  `m5stack/M5GFX`), `usesExternalBus() == true`. Gated by `FREEINK_DRIVER_LGFX_EPD`
  (derived from `FREEINK_DEVICE_LILYGO`), so M5GFX links only on this device. It
  drives an 8-bit grayscale `LGFX_Sprite` canvas (PSRAM): B/W frames expand into
  it, and the **16-gray reading path** is implemented (`displayGray` +
  `copyGrayscaleLsb/Msb` + `writeGrayscalePlaneStrip` combine the base + LSB/MSB
  planes into 4 levels, matching the reference port), then `pushSprite` runs the
  EPD refresh at the requested `epd_mode`.
- **`BoardConfig::LILYGO_T5S3`** profile — geometry, GT911 touch
  (`LILYGO_T5_PRO_GT911`), PWM backlight, I²C battery gauge (BQ27220/BQ25896),
  `DisplayController::LgfxEpd`. `CAP_TOUCH`/`CAP_FRONTLIGHT`/gauge auto-on.
- **GT911 touch**, **PWM backlight** (`FrontlightManager`), and the **I²C fuel
  gauge** (`BatteryMonitor`) are all wired.

> Status: the full stack (display incl. grayscale, touch, backlight, battery)
> compiles and follows the validated reference port, but has **not been run on
> T5 S3 hardware** by the SDK author. Treat as "implemented, pending on-device
> validation."

## Board bring-up (what the consumer supplies)

The panel can't power up without the board's PMIC (TPS65185) + PCA9535 I²C
IO-expander sequence — that glue is board-specific, so `LgfxEpdDriver` has **no
default config**. The board injects everything LovyanGFX needs through an
`LgfxEpdConfig`:

```cpp
namespace freeink {
const LgfxEpdConfig& lilygoT5S3LgfxConfig() {
  static const LgfxEpdConfig cfg = {
      {EP_D0,EP_D1,EP_D2,EP_D3,EP_D4,EP_D5,EP_D6,EP_D7},  // 8-bit data bus
      EP_STH, EP_STV, /*OE dummy*/ T5S3_LORA_CS, EP_LEH, EP_CKH, EP_CKV,
      /*pwr dummy*/ T5S3_LORA_CS, /*busHz*/ 20'000'000, /*linePadding*/ 8,
      /*rotation*/ 1,
      { &prepareEpdPowerPins, &epdPowerOn, &epdPowerOff },  // PCA9535 + TPS65185 glue
  };
  return cfg;
}
}
```
Build with `-DFREEINK_DEVICE_LILYGO=1 -DFREEINK_LGFX_EPD_CONFIG=lilygoT5S3LgfxConfig`
and add `m5stack/M5GFX` to that env's `lib_deps` (see `platformio.sample.ini`). The
power-hook bodies (PCA9535 expander + TPS65185 PMIC register writes) live in the
board-support layer, not the SDK.

A **complete, ready board-support implementation** of this config (the real
parallel pins + the PCA9535/TPS65185 power sequence, reusing the board's expander
helpers) lives in the reference port at
**`lib/Board_T5S3/FreeInkLgfxConfig.cpp`** — copy it as the starting point for a
T5 S3 build against this SDK.

## What the SDK now wires (vs. the reference fork)

- **Battery** — `BatteryMonitor` now has an **I²C fuel-gauge backend**
  (`FREEINK_BATTERY_I2C_GAUGE`, auto-on for LilyGo): reads SoC/voltage from the
  **BQ27220** and charge status from the **BQ25896**, addresses/pins from
  `BoardProfile.batteryGauge`. Same public API as the ADC path — drop-in. Minimal
  raw-register read, no external lib.
- **Backlight** — wired in the profile as a PWM `FrontlightConfig` (BL_EN GPIO11,
  driven by `FrontlightManager`); `CAP_FRONTLIGHT` auto-on.
- **Expander button** — `InputManager::setButtonHook()` lets the board feed
  buttons that aren't direct GPIOs; the board reads the **PCA9535** and returns a
  `BTN_*` bitmask, so InputManager stays device-agnostic. (The board supplies the
  PCA9535 read itself — that part is board-support.)

## Remaining board-support (not core SDK)

- **PCA9535 expander, TPS65185 PMIC** — the EPD power sequence (injected via
  `LgfxEpdConfig::power`) and the expander-button read (via `setButtonHook`) live
  in the board layer.
- **`PowerManager` deep-sleep wake** assumes a direct GPIO; a button only reachable
  through the expander INT isn't a wake source yet.
- **PCF85063 RTC / LoRa / GPS** — board-support; the SDK doesn't absorb them.
- **GT911 home key** — our GT911 backend polls the touch status but doesn't yet
  surface the capacitive home-key bit (minor; map it via `setButtonHook` for now).
