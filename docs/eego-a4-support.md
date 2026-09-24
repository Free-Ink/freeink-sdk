# EEGO A4 board support

Status: **implemented; hardware validation remains incomplete.** The SDK includes
an EEGO A4 board profile, display driver, touch backend, and frontlight backend.
The original pin map and controller details came from stock-firmware analysis
(see [Provenance](#provenance)); outstanding checks are listed below.

## Device identity

- Product: **EEGO Reader A4** (`eego_a4`, board tag `EA04`). Chinese-market e-reader.
- Firmware examined: `EEGOREAD_A4_130.bin` (5,816,736 bytes), app label
  `EegoRead-ESP32-1.3.0`, built with PlatformIO / Arduino-ESP32, ESP-IDF v5.5.4,
  compiled 2026-06-02.
- **Lineage: this is a CrossPoint fork.** The stock firmware is a rebrand of the
  CrossPoint reader from a snapshot around **May–early June 2026**, *before*
  CrossPoint moved onto the FreeInk SDK — so the shipped binary rides the older
  community-sdk, not this SDK. Proof: identical source-tree paths
  (`src/activities/reader/KOReaderSyncActivity.cpp`, the `EpubReaderActivity`
  sub-menus), the `Hal*` / `ActivityManager` / `GfxRenderer` architecture, and a
  byte-for-byte match of the `appendSegmentPatternBreaks(...)` hyphenator symbol
  to CrossPoint `lib/Epub/Epub/hyphenation/Hyphenator.cpp`. The CrossPoint name is
  stripped from the strings, so grep-by-name does not find it — compare
  architecture and symbols instead. OEM additions on top: a Tencent tcloudbase
  cloud push (`CloudPushActivity`), a WeChat-QR pairing + `/eego/upload*` local
  transfer server (`LocalTransferServer`), an `EEFONT`/`FONT_PACK` font protocol,
  and the A4 panel/touch drivers.

## Hardware

| Item | Value |
|------|-------|
| SoC | ESP32-S3 **N16R8** (16 MB DIO flash, 8 MB OPI PSRAM) |
| Panel | **UC8279C**, **768 × 552** (2 bpp), BUSY active-low |
| Panel RAM | 768 × **600**; host FB is 768×552 sent **bottom-up**, then 48 white rows |
| Framebuffer | 52,992 bytes (768/8 × 552); grayscale planes 52,992 B each, lazy from PSRAM |
| Display SPI | SCLK 42, MOSI 45, CS 21, DC 14, RST 13, BUSY 41, PWR-EN 6 · **20 MHz** |
| MicroSD | dedicated **HSPI** bus: SCLK 39, MISO 40, MOSI 38, CS 47 · 20 MHz |
| Buttons | UP 5 and DOWN 7 active-low; POWER 8 active-high with INPUT_PULLDOWN |
| Battery | ADC GPIO 10, charge-status GPIO 11, divider ×1.559 |
| Touch | **GSLX680** — SDA 2, SCL 1, RST 3, addr 0x40. Firmware blob uploaded at boot (`gsl/EegoA4GslFirmware.h`, extracted + hash-verified); backend in InputManager. |
| Touch mapping | Calibration in `pollGslx680` returns panel-native X 0..767 / Y 0..551; profile swap/flip disabled |
| Screen key | GSL sentinel `rawX=0x03a0, rawY=0x1020`; short press = Back, 700 ms hold = Home |
| RTC | **PCF8563** at 0x51, on the **shared touch I2C bus** (SDA 2 / SCL 1), 400 kHz |
| Power latch | GPIO 4 (held to stay powered) |
| Deep sleep | send controller `0xE0 = 0x88`, float SDA/SCL, hold GPIO 3 (touch RST) low |
| Frontlight | Optional LM3630A at I²C 0x36 on SDA 2 / SCL 1, enable GPIO 12; runtime-probed by `FrontlightManager` through `BoardProfile::i2cFrontlight`. |
| UI scale | 1.2 |

### Refresh behaviour

At most **four** consecutive fast refreshes; the fifth forces a full refresh.
Grayscale is rendered from two lazily-allocated PSRAM planes (LSB/MSB); if
allocation fails the driver leaves the existing B/W image up and never falls back
to internal DRAM.

## SDK integration

Build with `-DFREEINK_DEVICE_EEGO_A4=1`; see the `eego_a4` environment in
[platformio.sample.ini](../platformio.sample.ini).

- `BoardConfig::EEGO_A4` selects the UC8279C driver, digital buttons, GSLX680
  touch, PCF8563 RTC, dedicated HSPI SD bus, and GPIO4 power latch.
- `Uc8279cA4Driver` uses the shared `PanelDriver` / `EpdBus` interfaces with
  external Full/Fast/Gray LUTs, bottom-up scan, and padding to 600 panel RAM rows.
- `InputManager` uploads the GSLX680 firmware and applies calibration in
  `pollGslx680`, returning panel-native coordinates without a second profile transform.
- `FrontlightManager` probes the optional LM3630A through
  `BoardProfile::i2cFrontlight`. The profile's PWM frontlight field is `NO_FRONTLIGHT`.

## Open items (need a physical unit)

- Confirm 768×552 on real glass (ghosting, gray levels). The UC8279C bring-up now
  does the full power/booster/VCOM/PLL setup and uploads external Full/Fast/Gray
  LUTs (hash-verified), so refreshes actually run — validate quality on a unit.
- Validate the LM3630A frontlight init sequence and brightness curve on a frontlit
  unit, and runtime absence handling on a unit without the chip.
- Re-verify the whole pin map on hardware: a frontlit-unit dump did NOT contain the
  `20 MHz` display clock or the `eego_a4` name the scaffold assumed (both units are
  pre-freeink community-sdk builds), so the borrowed pin values are provisional.
- Confirm navigation GPIOs 5/7 and battery divider ×1.559. The current profile
  sets POWER GPIO8 active-high with `INPUT_PULLDOWN`; an internal pull-up caused
  phantom presses. Charge-status GPIO11 is active-high in the profile.
- Validate GSLX680 touch on hardware: the firmware blob is byte-verified (SHA-256
  `076ac8…`) and the init/read sequence is RE-derived, but the
  panel-native calibration needs a corner-tap check.
- Standby current with the GPIO4 latch + GPIO3-low sequence.

## Provenance

The initial hardware map came from reverse-engineering the stock OEM firmware
`EEGOREAD_A4_130.bin`: the CrossPoint lineage, ESP32-S3, board tag `EA04`, the
UC8279C controller, 768×552 resolution, pin map, and touch/RTC calibration. Later corrections are reflected in the
current board profile. See
[Open items](#open-items-need-a-physical-unit) for the remaining hardware checks.
