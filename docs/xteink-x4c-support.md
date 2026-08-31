# Xteink X4 Classic (X4C)

ESP32-S3 (16 MB flash, 8 MB PSRAM) e-reader. 800×480 B/W panel, **button-only**
navigation — **no touchscreen and no frontlight**. It shares the ESP32-S3 board and
glass of the [Xteink X4 Pro](xteink-x4pro-support.md) and uses the same display
driver stack, so most of that document's display detail applies; this page covers
the X4C-specific pin assignment and peripherals. Its profile is
`BoardConfig::XTEINK_X4_CLASSIC` (`Board::XteinkX4Classic`).

The panel controller varies by production unit — **SSD1677**, **UC8179**, or
**UC8279** — all driving the same 800×480 glass on the same X4C pinout. The X4C
display bus has no MISO, so the SDK reads the factory `hw_calib/screenType` NVS
value at boot and selects the matching driver.

Build: `-DFREEINK_DEVICE_X4CLASSIC=1` (see `platformio.sample.ini` `[env:x4c]`).
`FREEINK_DRIVER_SSD1677`, `FREEINK_DRIVER_UC8179`, `FREEINK_DRIVER_UC8279_X4`,
`FREEINK_CAP_RTC`, `FREEINK_BATTERY_I2C_GAUGE`, and `FREEINK_SD_SDMMC` auto-enable.
`FREEINK_CAP_TOUCH` and `FREEINK_CAP_FRONTLIGHT` stay **off**. The SD path also
requires `USE_BLOCK_DEVICE_INTERFACE=1` in the consumer build (the `x4c` env
defines it).

## Board identity

- Board tag `ESP32S3_X4_CLA` (revision `ESP32S3_X4R2_CLA`), default panel string
  `ESP32S3_X4_CLA_SSD1677`.
- Panel-controller selection: NVS namespace `hw_calib`, key `screenType` (u8:
  1 = UC8179, 2 = UC8279, 3/default = SSD1677). Because the bus has no MISO, this
  NVS value is authoritative — the SDK maps it directly to the driver at boot.

## Display — 800×480

| Signal | GPIO | Notes |
|--------|------|-------|
| SCLK   | 12   | |
| MOSI   | 11   | write-only, no MISO |
| CS     | 13   | |
| DC     | 14   | command = LOW, data = HIGH |
| RST    | 10   | active-low reset pulse |
| BUSY   | 18   | input, active-high |

`GPIO1` is the master peripheral/panel power rail, driven HIGH at boot. `GPIO6` is
**not** a display pin — it is the SD card's power enable (see Storage). No custom
LUT, drive voltages, or external PMIC are needed; the panel runs the same UC8279
command stream and OTP waveform as the X4 Pro. The X4C omits the UC8279 PLL (0x30)
command that the X4 Pro programs. `displaySpiHz` is 20 MHz.

## Input — eight discrete buttons, no touch

Every key is a dedicated active-low GPIO. Seven are interrupt-driven buttons; GPIO4
is a plain input (not a button, not used by the SDK). The layout is the two side
keys of the X4 Pro plus the four bottom keys of the C3 X4:

- **Two side keys (page turn):** Left = GPIO0 → up/previous, Right = GPIO7 →
  down/next. Power = GPIO3.
- **Four bottom keys:** GPIO5 = Left, GPIO2 = Right, GPIO8 = Confirm, GPIO9 = Back.

Because the X4C is a buttons-only device (`InputStyle::DigitalButtons` +
`NO_TOUCH`) with all six navigation actions mapped, a consumer UI that reads the
profile shows side-bezel button hints, like the C3 X4/X3.

## Touch — none

No touchscreen. Profile: `NO_TOUCH`.

## Frontlight — none

No frontlight. GPIO8/GPIO9 (the X4 Pro's warm/cool PWM channels) are button inputs
here. Profile: `NO_FRONTLIGHT`.

## I²C sensors — SDA 39 / SCL 38 @ 400 kHz

One shared master bus carries three devices:

- **RTC:** BM8563 (PCF8563-register-compatible) at 0x51.
- **Battery gauge:** CW2017 at 0x63. The gauge needs its 80-byte BATINFO profile
  uploaded before it reports SoC; `BatteryMonitor` handles this.
- **IMU:** QMI8658 six-axis accelerometer/gyro at 0x6B (WHO_AM_I = 0x05), no
  interrupt line. Profile: `ImuType::Qmi8658`.

## Battery charge status

GPIO21 is the charge `STAT` input, active-high (`batteryChargeStatus = 21`,
`batteryChargeStatusActiveHigh = true`).

## Storage — SD card (native SDMMC)

Native SDMMC, 1-bit: CLK = 41, CMD = 42, DAT0 = 40, slot 1. `GPIO6` is the card's
active-low power enable: it is driven HIGH for 80 ms, then LOW, and held LOW while
the card is mounted and accessed. The SDK carries GPIO6 as `sd.powerEnable`
(`powerActiveHigh = false`); the native SDMMC block device performs the HIGH→LOW
power cycle and retries it on mount failure.

## Other peripherals

- **USB:** native ESP32-S3 USB-OTG (D− = GPIO19, D+ = GPIO20), USB-MSC capable.
- No audio codec, buzzer, LED, PMIC, or GPIO expander. Power management is
  direct-GPIO (GPIO1 rail) plus the CW2017 gauge and BM8563 RTC.
- WiFi and BLE radios are present.

## Partitions (16 MB, dual-OTA)

`nvs` @ 0x9000, `otadata` @ 0xE000, `app0` @ 0x10000, `app1` @ 0x7F0000, `spiffs`
@ 0xFD0000, `coredump` @ 0xFE4000.
