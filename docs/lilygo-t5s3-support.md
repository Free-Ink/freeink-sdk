# LilyGo T5 S3 (4.7" ED047TC1)

The LilyGo T5S3-4.7-ePaper (PRO/Lite) is an ESP32-S3 board. Its panel is an
**ED047TC1: a raw 960×540 16-gray parallel EPD with no on-glass controller**. The
MCU clocks every row over the S3 LCD (i80) peripheral and an external PMIC
generates the waveform rails — a different display class from FreeInk's SPI
single-chip drivers (SSD1677/UC8253/ED2208).

Reference port: [ShallowGreen123/t5s3-reader](https://github.com/ShallowGreen123/t5s3-reader),
whose `HalDisplay` delegates to the SDK's `EInkDisplay`/`LgfxEpdDriver`.

## Display

`LgfxEpdDriver` wraps **LovyanGFX's `Panel_EPD`/`Bus_EPD`** (bundled in
`m5stack/M5GFX`) and reports `usesExternalBus() == true`. It compiles under
`FREEINK_DRIVER_LGFX_EPD` (derived from `FREEINK_DEVICE_LILYGO`), so M5GFX links
only on this device. The driver holds an 8-bit grayscale `LGFX_Sprite` canvas in
PSRAM:

- `display()` expands the 1-bpp framebuffer into the canvas and `pushSprite`s it
  at the requested `epd_mode` (FULL→quality, HALF→text, FAST→fast).
- The 16-gray path (`displayGray` + `copyGrayscaleLsb/Msb` +
  `writeGrayscalePlaneStrip`) combines the B/W base with the LSB/MSB planes into
  four gray levels in the canvas, then `pushSprite`s at the quality waveform.

`BoardConfig::LILYGO_T5S3` carries the geometry, `DisplayController::LgfxEpd`, the
GT911 touch config, the PWM backlight, and the I²C battery gauge.

## Board configuration

The panel's power comes from the board's PMIC (TPS65185) and a PCA9535 I²C
IO-expander, which is board-specific, so `LgfxEpdDriver` takes its pins and power
hooks from a board-supplied `LgfxEpdConfig`:

```cpp
namespace freeink {
const LgfxEpdConfig& lilygoT5S3LgfxConfig() {
  static const LgfxEpdConfig cfg = {
      {EP_D0,EP_D1,EP_D2,EP_D3,EP_D4,EP_D5,EP_D6,EP_D7},  // 8-bit data bus
      EP_STH, EP_STV, /*OE dummy*/ T5S3_LORA_CS, EP_LEH, EP_CKH, EP_CKV,
      /*pwr dummy*/ T5S3_LORA_CS, /*busHz*/ 16'000'000, /*linePadding*/ 8,
      /*rotation*/ 1,
      { &prepareEpdPower, &epdPowerOn, &epdPowerOff },  // PCA9535 + TPS65185 glue
  };
  return cfg;
}
}
```

A build sets `-DFREEINK_DEVICE_LILYGO=1 -DFREEINK_LGFX_EPD_CONFIG=lilygoT5S3LgfxConfig`
and adds `m5stack/M5GFX` to that env's `lib_deps` (see `platformio.sample.ini`). The
power-hook bodies (PCA9535 expander + TPS65185 PMIC register writes) live in the
board-support layer. A complete implementation — the real parallel pins and the
PCA9535/TPS65185 power sequence reusing the board's expander helpers — is in the
reference port at **`lib/Board_T5S3/FreeInkLgfxConfig.cpp`**.

## Peripherals

- **Touch** — GT911, handled by `InputManager` (polled, reset/address dance). The
  profile uses `BoardConfig::LILYGO_T5_PRO_GT911`.
- **Backlight** — PWM `FrontlightConfig` on BL_EN (GPIO11), driven by
  `FrontlightManager`; `CAP_FRONTLIGHT` is on for this device.
- **Battery** — `BatteryMonitor`'s I²C fuel-gauge backend
  (`FREEINK_BATTERY_I2C_GAUGE`) reads SoC/voltage from the BQ27220 and charge
  status from the BQ25896, with addresses/pins from `BoardProfile.batteryGauge`. It
  presents the same API as the ADC path and uses a minimal raw-register read (no
  external library).
- **Power button and sleep** — the power button is the BOOT button (GPIO0, a
  direct RTC-capable GPIO); the profile sets `input.power = GPIO0`. `InputManager`
  reads it, and `PowerManager::armPowerButtonWakeup()` arms deep-sleep wake on it
  with the per-SoC source (`ext1` on the S3). This matches the reference port,
  which wakes on the same BOOT button.
- **PCA9535 user button** — a second button behind the I²C expander.
  `InputManager::setButtonHook()` takes a board callback that reads the PCA9535 and
  returns a `BTN_*` bitmask, so `InputManager` carries no expander code. This
  button is not a deep-sleep wake source (neither is it in the reference port).

## Board-support (outside the SDK)

- **PCA9535 expander and TPS65185 PMIC** — the EPD power sequence (via
  `LgfxEpdConfig::power`) and the user-button read (via `setButtonHook`) both drive
  the same PCA9535, so the board owns the expander and feeds both seams.
- **GT911 home key** — the GT911 backend reads touch points but does not surface
  the capacitive home-key bit; a board maps it through `setButtonHook`.
- **PCF85063 RTC, LoRa, GPS** — board peripherals the SDK does not cover.
