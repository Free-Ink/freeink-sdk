# Xteink X4 Pro

ESP32-S3 (16 MB flash, 8 MB PSRAM) e-reader. **SSD1677 800×480 B/W** panel (same
controller as X4 / de-link / Sticky), **GT911** capacitive touch, and a **dual
warm/cold color-temperature frontlight**. Distinct device from the ESP32-C3
`XTEINK_X4` — it has its own S3 profile, `BoardConfig::XTEINK_X4_PRO`.

Build: `-DFREEINK_DEVICE_X4PRO=1` (see `platformio.sample.ini` `[env:x4pro]`).
`FREEINK_DRIVER_SSD1677`, `FREEINK_CAP_TOUCH`, and `FREEINK_CAP_FRONTLIGHT`
auto-enable.

## Display — SSD1677, 800×480

| Signal | GPIO |
|---|---|
| SCLK | 12 |
| MOSI | 11 |
| MISO | — (write-only) |
| CS | 18 |
| DC | 13 |
| RST | 14 |
| BUSY | 6 |
| Power-enable | unassigned (GPIO7 candidate) |

SPI clock 5 MHz. Native landscape scan (`NO_FLIP`). Uses the driver's default
X4/GDEQ0426T82 config.

## Touch — GT911

| Signal | GPIO |
|---|---|
| I²C SDA | 39 |
| I²C SCL | 38 |
| INT | 21 |
| RST | 4 |

Address 0x5D (alt 0x14), 400 kHz. Reports pixel coordinates (raw range = 800×480),
standard 8-byte frame (track-id in byte 0, `gt911CoordsAtByte0 = false`).

## Frontlight — dual warm/cold PWM

Two LEDC channels, mixed by `FrontlightManager` (`setBrightness` = total level,
`setColorTemperature` = warm/cool split).

| Channel | GPIO |
|---|---|
| Cool (`frontlight.gpio`) | 8 |
| Warm (`frontlight.gpioWarm`) | 9 |

10 kHz, 10-bit, active-high.

## Input — ADC resistor ladder

`InputStyle::XteinkAdcLadder` (NVS keys `adcOK`/`adcBACK`/`adcUP`/`adcDOWN`).
Power button GPIO3, active-low. Ladder ADC pins default to GPIO1/GPIO2
(`InputManager`).

## Storage / power

- MicroSD on a second SPI bus: SCLK 41 / MISO 40 / MOSI 42. Exposed over USB-MSC.
- Battery monitoring present.

## Partitions (16 MB, dual-OTA)

| Label | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x009000 | 0x005000 |
| otadata | data/ota | 0x00E000 | 0x002000 |
| app0 | app/ota_0 | 0x010000 | 0x7E0000 |
| app1 | app/ota_1 | 0x7F0000 | 0x7E0000 |
| spiffs | data/spiffs | 0xFD0000 | 0x014000 |
| coredump | data/coredump | 0xFE4000 | 0x01C000 |

## Unverified — confirm on hardware

- Panel and touch mount orientation (ships `NO_FLIP`, no touch swap/flip).
- Warm-vs-cool pin identity (GPIO8 vs GPIO9) — if reversed, CT direction inverts.
- SD CS + power-enable pins (SD stays dormant until CS is set).
- Battery ADC pin + divider; USB/VBUS-detect pin (GPIO10 candidate).
- Display power-enable (GPIO7 candidate).
- ADC-ladder pins (GPIO1/GPIO2 default) and ladder thresholds.
