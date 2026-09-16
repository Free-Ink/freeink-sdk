# Metalio E-Ink 4

Select `-DFREEINK_DEVICE_METALIO_EINK4=1`, or copy the `metalio_eink4`
environment from [platformio.sample.ini](../platformio.sample.ini).
The reference board uses an ESP32-S3, 16 MB flash, and 8 MB octal PSRAM.
Support is compiled and host-tested; physical hardware validation is pending.

## Supported peripherals

| Peripheral | SDK support |
|---|---|
| GDEM0397T81, 800×480 SSD1677 | Full, half, fast, windowed and deferred B/W refresh |
| CST816S touch | Tap, swipe, hold, native-coordinate mapping and cover keys |
| TCA9555 | Board power/reset sequencing and volume-button input |
| microSD | Native 1-bit SDMMC through SDCardManager |
| BQ27220, address 0x55 | Battery percentage, voltage and charge direction via BatteryMonitor |
| PCF8563, address 0x51 | Rtc |
| GPIO44 motor | [HapticManager](haptics.md): consumer-triggered intensity, pulses and patterns |
| Speaker | AudioManager: 16 kHz, 16-bit PCM WAV; software volume and looping |
| SC7A20H accelerometer | Imu: XYZ acceleration in g, ±2 g / 100 Hz, sleep/wake |
| Microphone | Microphone: 16 kHz mono PCM capture, shared duplex I²S |
| Power controller | Explicit `freeink::metalio::powerOff()` pulse helper |

Touch, RTC, haptics, audio, microphone, IMU, the battery gauge and SDMMC capabilities enable automatically.
Haptic playback is explicitly requested by the consumer; touch does not trigger it.
Set `USE_BLOCK_DEVICE_INTERFACE=1` for SdFat's SDMMC block-device interface.
There is no frontlight in the supplied board configuration.

The 4G modem, charger configuration and expander LED have no Metalio SDK backend
in this port. `FREEINK_CAP_LED` remains off.

The SC7A20H identity was confirmed by the board owner. Its
[Imu backend](metalio-accelerometer.md) probes 0x19 then 0x18 on the shared
SDA41/SCL42 bus. This is an accelerometer-only device: gyro readings are zero.
The interrupt on expander P1.4 is not used by the polling backend.

The [audio backend](metalio-audio.md) controls the external module over UART
and streams slave I²S. Bluetooth pairing/routing features are not exposed. The charger is left in its existing hardware state;
charge direction is read from the gauge, not from a guessed charger register map.

## Pins and source discrepancies

| Function | Wiring |
|---|---|
| EPD | SCLK14, MOSI8, CS45, DC13, RST18, BUSY9 (active high), 10 MHz |
| Shared I²C | SDA41, SCL42, 400 kHz |
| Touch | 0x15, IRQ1, reset on TCA9555 P1.1 |
| SDMMC | CLK38, CMD40, D0=39; 1-bit |
| Physical buttons | BOOT0 → confirm, POWER3 → power, both active low |
| Expander buttons | P0.7 → down, P1.0 → up, active low |
| Expander outputs | P0.5 screen/card power, P0.6 main power, P1.1 touch reset, P1.3 shutdown pulse |
| Audio controls | P0.4 amp enable, P0.1 route (low selects ESP32) |

The supplied **Metalio GPIO Pin Definition.xlsx**, sheet1 rows 30–31, says
DC18/RST13. The **metalio-hw-test v2.0.51** `config.h` says DC13/RST18;
this port follows the executable sample. Confirm this mapping on the actual board
before assuming a nonresponsive panel is a waveform problem.

The workbook's `BUSY_N` label differs from the sample's active-high BUSY loop;
the port follows the sample. GPIO44 is called UART RX in the workbook but is the
vibration output in the sample. Do not enable a UART receiver on that pin while
using the motor. Expander names such as P11 mean **P1.1 (bit 9)**, not bit 11.

GPIO46 is SD DAT3 and input-only. It is not assigned as a data/output pin in
1-bit mode; the board must keep DAT3 high through its pull-up so the card does
not enter SPI mode.

## Startup, touch and power

Call `BoardConfig::holdPowerRails()` early in setup. Display, input and SD
initialization also call the idempotent board helper. It sets output levels
before changing expander directions, enables main and screen/card power,
releases touch reset after 10 ms, and waits 120 ms. The amplifier and motor
start off. Failed initialization can be retried.

The sample rotates its 480×800 logical display onto the 800×480 panel with
`panelX = touchY`, `panelY = 479 - touchX`. FreeInk applies that transform
before the renderer's orientation mapping.

Cover keys are kept out of screen taps:

- HOME `(80,900)` emits the existing `wasHomeKeyPressed()`,
  `wasHomeKeyTapped()` and `wasHomeKeyLongPressed()` events.
- NEXT `(240,900)` emits `BTN_DOWN`; PREV `(400,900)` emits `BTN_UP`.
- Physical volume +/- also emit `BTN_UP`/`BTN_DOWN`.

The CST816S may NACK until touched. Initialization therefore does not require
an ID-register response; IRQ pulses are latched and expired contacts/failed
reads release input. See [Espressif's CST816S notes](https://github.com/espressif/esp-bsp/blob/master/components/lcd_touch/esp_lcd_touch_cst816s/README.md).

For hardware power-off, call `HapticManager::getInstance().end()` if using haptics,
finish all display work, call `display.deepSleep()`,
close/unmount storage, then call `freeink::metalio::powerOff()` from your hardware
task. Call `AudioManager::powerDown()` and `Microphone::end()` before sleeping
or pulsing hardware power-off. This disables the amp, waits 280 ms, and issues one high/low/high shutdown
pulse with 100 ms intervals. The caller may repeat the pulse if USB keeps the
device powered. Do not cut P0.5 first: it is shared by the screen and card, and
the sample explicitly keeps it powered during panel shutdown. `powerOff()` does
not replace application-level display/storage shutdown. Board helpers use the
shared Wire bus; serialize board-control calls with your input/hardware task.

`PowerManager` supports GPIO3 deep-sleep wake, but deep sleep retains the shared
rail. Measure standby current on hardware; this is distinct from pulsing the
external power controller.

## Refresh and waveforms

Default refresh settings follow the sample's internal controller waveforms:

| Mode | Update register 0x22 | Border 0x3C |
|---|---|---|
| Full | 0xF7 | 0x01 |
| Half | 0xD7, temperature 0x6A | 0x01 |
| Fast / window | 0xFC | 0x80 |

A first FAST request becomes HALF to establish the initial screen contents.
Full/fast restore internal temperature sensing after HALF. A requested power-down
after 0xFC occurs after BUSY completes, including deferred refreshes. Deep sleep
uses mode 0x03. Grayscale is not advertised, and legacy grayscale calls without
a supplied LUT fall back to B/W.

The separately supplied [waveform document](metalio-waveform-source.md) is
preserved for bring-up. Its full-refresh `kEpdLutPartial` has 112 values, but its
partial-refresh `kEpdLutFastUpdate[112]` has **113 initializers**. The extra entry
precedes the frame-rate/voltage tail: applying the SDK's 105-byte LUT split would
put `0x22` into the gate-voltage register instead of `0x17`. Neither custom table
is enabled by default. Obtain a corrected partial table and validate the drive
sequence before replacing the sample's working OTP path; do not silently drop
a byte. These B/W tables also do not establish grayscale support.

## Verification

```sh
sh libs/hardware/InputManager/test/host/run_metalio.sh
sh libs/hardware/AudioManager/test/host/run_metalio.sh
sh libs/hardware/Imu/test/host/run.sh
python3 libs/display/FreeInkDisplay/test/host/run_pro.py
```

Tests exercise the actual board profile, expander sequencing/retries, touch and
cover keys, input failures, refresh commands, B/W fallback and deferred shutdown.
An Arduino ESP32-S3 firmware linking display, input, SD, battery, RTC and power
managers was also compiled. Before release, validate DC/RST wiring, panel quality,
all touch corners and held cover keys, SD access, battery values, RTC, accelerometer
axis orientation and sleep current, and power
off/wake on a physical unit.
