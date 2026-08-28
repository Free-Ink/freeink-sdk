# EEGO A4 board support

Status: **planning / not yet hardware-validated.** This document captures what is
known about the EEGO Reader A4 and the plan to add it to the FreeInk SDK. Pins,
resolution, and the panel controller are recovered by reverse-engineering the
stock OEM firmware (see [Provenance](#provenance)); nothing below is confirmed on
a physical unit yet.

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
| Buttons | UP 5, DOWN 7, POWER 8 — plain active-high digital buttons |
| Battery | ADC GPIO 10, charge-status GPIO 11, divider ×1.559 |
| Touch | **GSLX680** — SDA 2, SCL 1, RST 3, addr 0x40. Firmware blob uploaded at boot (`gsl/EegoA4GslFirmware.h`, extracted + hash-verified); backend in InputManager. |
| Touch mapping | rawY 12..632 → display X; rawX 884..9 → display Y (reversed): swapXY + flipY |
| Screen key | GSL sentinel `rawX=0x03a0, rawY=0x1020`; short press = Back, 700 ms hold = Home |
| RTC | **PCF8563** at 0x51, on the **shared touch I2C bus** (SDA 2 / SCL 1), 400 kHz |
| Power latch | GPIO 4 (held to stay powered) |
| Deep sleep | send controller `0xE0 = 0x88`, float SDA/SCL, hold GPIO 3 (touch RST) low |
| Frontlight | **variant-dependent — two A4 versions exist, one frontlit and one not.** The frontlit version's light is a **warm/cool I²C LED-driver chip** (NOT LEDC PWM): driver at **I²C 0x36** on the shared `Wire` bus (with touch 0x40 / RTC 0x51), **enable = GPIO 12** (active-high), **warm = reg 4**, **cold = reg 3**, total clamped to 150. Driven by FrontlightManager's `viaI2cLed` backend. |
| UI scale | 1.2 |

### Refresh behaviour

At most **four** consecutive fast refreshes; the fifth forces a full refresh.
Grayscale is rendered from two lazily-allocated PSRAM planes (LSB/MSB); if
allocation fails the driver leaves the existing B/W image up and never falls back
to internal DRAM.

## SDK integration plan

The panel is a UC8279C, an UltraChip KW-family controller — the **same family as
this SDK's existing `Uc8279Driver` / `Uc8279X4Driver` / `Uc8179Driver`** — and it
rides the SDK's normal `PanelDriver` + `EpdBus` (SPI) abstraction, **not** a
raw-parallel path. So integration is a new sibling driver plus a board profile,
not new infrastructure:

1. **BoardConfig.h**: add `FREEINK_DEVICE_EEGO_A4` (ESP32-S3 family), a
   `DisplayController::UC8279C` enum value, `TouchController::Gslx680`,
   `RtcType::Pcf8563` (already present), and the `EEGO_A4` `BoardProfile` with the
   pins above. Derive `FREEINK_DRIVER_UC8279C`, `FREEINK_CAP_TOUCH`,
   `FREEINK_CAP_RTC`, and a dedicated-HSPI SD path.
2. **Driver**: `Uc8279cA4Driver` (`begin`/`display`/`displayGray`/`deepSleep`) +
   its full/fast/gray LUT tables, modelled on `Uc8279X4Driver`. Handles the
   768→600 RAM padding, bottom-up scan, 4-fast-then-full cadence, and lazy PSRAM
   gray planes.
3. **InputManager**: GSLX680 touch backend (`beginGslx680`/`pollGslx680`) — resets
   the chip, uploads `gsl/EegoA4GslFirmware.h`, verifies the 0x5A5A5A5A load magic,
   then reads reg 0x80 with the GSL nibble-unpack. Digital UP/DOWN/POWER buttons.
4. **Rtc / SDCardManager / PowerManager**: PCF8563 on the shared bus; HSPI SD
   bus; GPIO4 latch + GPIO3-low deep-sleep sequence.

## Open items (need a physical unit)

- Confirm 768×552 on real glass (ghosting, gray levels). The UC8279C bring-up now
  does the full power/booster/VCOM/PLL setup and uploads external Full/Fast/Gray
  LUTs (hash-verified), so refreshes actually run — validate quality on a unit.
- Validate the **I²C LED-driver frontlight** on hardware (backend implemented:
  `FrontlightConfig.viaI2cLed`, chip 0x36, enable GPIO 12, cool=reg3 / warm=reg4,
  total clamp 150). Confirm the init register sequence (`kI2cLedInit` in
  FrontlightManager.cpp is RE-observed and may need tuning) and the brightness curve.
- Decide how to model the two A4 variants (frontlit / not) — the non-frontlit unit
  would use `NO_FRONTLIGHT`.
- Re-verify the whole pin map on hardware: a frontlit-unit dump did NOT contain the
  `20 MHz` display clock or the `eego_a4` name the scaffold assumed (both units are
  pre-freeink community-sdk builds), so the borrowed pin values are provisional.
- Confirm button GPIOs 5/7/8 and battery divider ×1.559. Buttons are set **active-low**
  (INPUT_PULLUP) — active-high phantom-triggered the power-button sleep path on first
  boot. Active-low is also the safe default for an unused/floating pin.
- Validate GSLX680 touch on hardware: the firmware blob is byte-verified (SHA-256
  `076ac8…`) and the init/read sequence is RE-derived, but the coordinate
  orientation (swapXY/flipY) and calibration range need a corner-tap check.
- Standby current with the GPIO4 latch + GPIO3-low sequence.

## Provenance

Everything above comes from reverse-engineering the stock OEM firmware
`EEGOREAD_A4_130.bin`: the CrossPoint lineage, ESP32-S3, board tag `EA04`, the
UC8279C controller, 768×552 resolution, pin map, and touch/RTC calibration. None
of it is confirmed on a physical unit yet — see [Open items](#open-items-need-a-physical-unit).
