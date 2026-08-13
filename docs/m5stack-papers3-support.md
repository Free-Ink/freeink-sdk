# M5Stack PaperS3 (4.7" ED047TC1)

The M5Stack PaperS3 is an ESP32-S3R8 board (16 MB flash / 8 MB octal PSRAM) — the
S3 successor to the M5Paper v1.1, carrying the same **ED047TC1 960×540 16-gray
glass** but with **no IT8951 timing controller**: the S3 clocks the panel
directly over an 8-bit parallel bus, the same display class as the LilyGo T5 S3.

Pin map sources: M5GFX autodetect (`board_M5PaperS3` in `src/M5GFX.cpp`),
M5Unified (`Power_Class`, pin tables), the M5PaperS3-UserDemo HAL, and the
official docs pinmap. Where the docs table disagrees (it labels GPIO45 "PWR" and
omits GPIO16/GPIO46), M5GFX — the shipping vendor driver — is authoritative.

## Display

`LgfxEpdDriver` (LovyanGFX `Panel_EPD`/`Bus_EPD`, bundled in `m5stack/M5GFX`)
compiles under `FREEINK_DRIVER_LGFX_EPD`, derived from `FREEINK_DEVICE_PAPERS3`
just as it is from `FREEINK_DEVICE_LILYGO`. The parallel bus:

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| D0 | 6 | | SPH (XSTL) | 13 |
| D1 | 14 | | CL (XCL) | 16 |
| D2 | 7 | | LE (XLE) | 15 |
| D3 | 12 | | SPV | 17 |
| D4 | 9 | | CKV | 18 |
| D5 | 11 | | OE | 45 |
| D6 | 8 | | PWR (EPD rail) | 46 |
| D7 | 10 | | | |

Unlike the LilyGo (TPS65185 PMIC + PCA9535 expander), the PaperS3's EPD power is
just the OE/PWR/SPV GPIOs, sequenced by `Bus_EPD`'s **stock** `powerControl`
(on: OE → PWR → SPV with settling delays; off in reverse). The board's
`LgfxEpdConfig` (`freeink::m5PaperS3LgfxConfig()` in
`libs/hardware/BoardPaperS3`) therefore carries real bus pins and **null power
hooks** — `LgfxEpdDriver`'s bus wrapper delegates to the base class when a hook
is absent. Its `prepare` hook only parks the PWROFF pulse line (GPIO44) LOW.

Bus speed 16 MHz, line padding 8, `rotation = 0` (the SDK's native-landscape
convention, like the LilyGo; M5GFX ships the device portrait via
`offset_rotation=3`). **Pending hardware validation:** if landscape "up" comes
out inverted, set rotation 2 in the config.

M5GFX hard-requires OPI PSRAM on this board, so the env sets
`board_build.arduino.memory_type = qio_opi` and `-DBOARD_HAS_PSRAM`.

## Inputs — touch only

**There are no firmware-readable buttons.** The single side button feeds the
PMS150G power-latch chip (press = on, 2 s hold = hard off, 6 s = reset) and never
reaches an ESP32 GPIO. The profile's `InputPins` are all unassigned; paging and
all navigation must come from the GT911 (tap zones / gestures / the firmware's
touch paging). Firmware without touch page-turn support is unusable here.

- **GT911 touch** — internal I²C bus SDA=GPIO41 / SCL=GPIO42 (shared with the
  RTC and IMU), INT=GPIO48, **no reset wired** (the controller self-loads its
  config, like M5Paper v1.1 → `gt911CoordsAtByte0=true`, pending validation),
  addresses 0x5D/0x14. Portrait digitizer (540×960) on the landscape panel →
  `swapXY=true`, rawMax in post-swap order (959×539); `flipX=false, flipY=true`
  mirror the LilyGo/M5Paper defaults **pending a corner-tap test**.
- GPIO48 is **not an RTC IO** on the S3: touch cannot be a deep-sleep EXT wake
  source (M5Unified uses light-sleep GPIO wake instead).

## Power

- **No power latch for firmware to hold** — the PMS150G self-latches; the
  profile's `PowerConfig` is empty and `holdPowerRails()` is a no-op.
- **Software power-off** = `BoardPaperS3::powerOff()`: a 5× 50 ms low/high pulse
  train on GPIO44 (a single edge does not release the latch — per M5Unified's
  power-off path). Idle level is LOW.
- **Wake-from-off via RTC**: the BM8563's INT line feeds the PMS150G, not an
  ESP32 GPIO — set an RTC alarm before powering off.
- **USB detect** — GPIO5, HIGH = USB present (`BoardProfile.usbDetect`).

## Peripherals

- **Battery** — ADC on GPIO3 (ADC1_CH2), 2:1 divider (pending validation);
  charge status GPIO4, LOW = charging (LGS4056H). No I²C fuel gauge.
- **RTC** — BM8563 (PCF8563 register-compatible) at 0x51 on SDA41/SCL42, handled
  by the `Rtc` lib (`RtcType::Pcf8563`); `CAP_RTC` auto-on.
- **SD card** — SPI/SdFat: SCLK=GPIO39, MISO=GPIO40, MOSI=GPIO38, CS=GPIO47. No
  power-enable gate. (Not SDMMC.)
- **Buzzer** — LEDC tone on GPIO21 (`Buzzer` lib, `CAP_BUZZER` auto-on).
- **IMU** — BMI270 at 0x68 on the internal bus. Not a supported `ImuType` yet,
  so it is omitted from the profile's sensors; adding it means a BMI270 backend
  in the `Imu` lib.
- **Status LED** — a single PWM LED on GPIO0 (`PAPERS3_LED`). Not an addressable
  strip, so it is not in `LedConfig` — board-support/firmware drives it directly.
- **No frontlight, no microphone, no output codec.**
- **Grove Port A** — GPIO1 (SCL) / GPIO2 (SDA), external bus; not covered by the
  SDK.

## Build

```ini
[env:papers3]  ; see platformio.sample.ini
board = esp32-s3-devkitc1-n16r8
board_build.arduino.memory_type = qio_opi
build_flags = -DBOARD_HAS_PSRAM -DFREEINK_DEVICE_PAPERS3=1
lib_deps =
  BoardPaperS3=symlink://freeink-sdk/libs/hardware/BoardPaperS3
  m5stack/M5GFX @ 0.2.20
```

No `-DFREEINK_LGFX_EPD_CONFIG` is needed: `FREEINK_DEVICE_PAPERS3` selects
`m5PaperS3LgfxConfig()` as the driver's config (an explicit
`-DFREEINK_LGFX_EPD_CONFIG=yourCfg` still overrides it for custom boards).

## Pending hardware validation

- Panel rotation (0 vs 2) for the SDK's landscape convention.
- Touch `flipX`/`flipY` (corner-tap test) and `gt911CoordsAtByte0`.
- Battery divider ratio / ADC scaling.
- SD SPI at the 40 MHz SdFat default (drop `SdPins.spiHz` to 20 MHz if mounts
  are flaky).
