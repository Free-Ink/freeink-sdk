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

The charger configuration and expander LED have no Metalio SDK backend in this
port. `FREEINK_CAP_LED` remains off.

The NT26 4G modem has no power enable and runs whenever the board is on. Its
host link is the vendor's framed UART-ethernet protocol at 2 Mbaud; it does not
answer plain AT commands. The SDK does not drive its pins; its only other
control is 4G_RST on expander P0.3.
The Bluetooth audio module (UART2, TX48/RX47) also has no power enable or known
sleep command. Only its amplifier (P0.4) is switchable; it stays off unless
AudioManager plays.

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
1-bit mode. Before SDMMC initialization the SDK configures it as an input with
pull-up enabled and pull-down disabled, matching the vendor demo. This matters
because GPIO46 defaults to a weak pull-down and the unused DAT3 line must still
be high during card initialization. The startup log reports its level (expected
1). External board pull-ups remain preferable for reliable operation.

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
an ID-register response. IRQ pulses wake idle polling through
`touchContactPending()`; periodic reads recover from missed interrupts and
failed first reads. Expired contacts/failed reads release input. Consumers must
keep calling `update()` promptly; an IRQ alone cannot preserve a tap that ends
before the controller is read. See [Espressif's CST816S notes](https://github.com/espressif/esp-bsp/blob/master/components/lcd_touch/esp_lcd_touch_cst816s/README.md).

For hardware power-off, call `HapticManager::getInstance().end()` if using haptics,
finish all display work, call `display.deepSleep()`,
close/unmount storage, then call `freeink::metalio::powerOff()` from your hardware
task. Call `AudioManager::powerDown()` and `Microphone::end()` before sleeping
or pulsing hardware power-off. Following the vendor firmware, it disables the amp,
holds the main and screen/card rails on, and toggles the power key (P1.3) every
100 ms until the power-switch chip cuts power. A single pulse, or a few spaced
ones, does not switch it off: measured on hardware, the board then stays powered
in deep sleep. On battery the call does not return; it returns after `maxMs`
(default 5 s) when USB keeps the device powered. Do not cut P0.5 first: it is
shared by the screen and card, and the sample explicitly keeps it powered during
panel shutdown. `powerOff()` does
not replace application-level display/storage shutdown. Board helpers use the
shared Wire bus; serialize board-control calls with your input/hardware task.

ESP32 deep sleep alone does not power the board down: the main and screen/SD
rails, the 4G modem and the audio module stay powered. To turn the board off,
call `powerOff()`, which pulses the power-switch chip. GPIO3 is that chip's
button and cold-boots the board. If USB keeps power present, fall back to
`PowerManager` deep sleep, which wakes on GPIO3.

## Refresh and waveforms

Full refresh uses the controller's internal waveform. Fast, window and half
refreshes use the supplied `kEpdLutPartial` table, loaded as an external LUT:

| Mode | Waveform | Update register 0x22 | Border 0x3C |
|---|---|---|---|
| Full | Internal | 0xF7 | 0x01 |
| Fast / window | `lut_fast_metalio` | 0xCC | 0x80 |
| Half | Two fast updates: previous → black → target | 0xCC | 0x80 |

`lut_fast_metalio` is the first 110 bytes of `kEpdLutPartial` from the
[waveform document](metalio-waveform-source.md). It is a single-pass differential
waveform (group 0: 3/16/1/1 frames at frame-rate byte 0x22) that drives changed
pixels to their target colour and gives unchanged pixels a short same-colour
pulse. Voltages: VGH 0x17, VSH1 0x41, VSH2 0xA8, VSL 0x32, VCOM 0x10.

A first FAST request becomes HALF to establish the initial screen contents.
HALF follows `lv_adapter_display.cc::PeriodicBlackPulseClear` in the demo.
Both controller image-memory planes are synchronized after completion, including
deferred updates, so the next update starts with the displayed image as its
baseline. A requested power-down is folded into the fast activation (0xCF).
Deep sleep uses mode 0x03. Grayscale anti-aliasing and four-tone images use the
separate grayscale LUTs and are unaffected.

The document's other table, `kEpdLutFastUpdate`, has 113 initializers. The extra
`0x01` is in group 9, whose phase durations are all zero, so either copy can be
dropped without changing the waveform. That table drives every pixel to the
opposite colour and back (about 72 frames) and is not used.

## USB SD-card export

CrossPoint's `metalio_eink4` target enables `FREEINK_CAP_USB_MSC=1`, using the
same USB Drive workflow as X4 Pro. Select **File Transfer → USB Drive** to expose
the SD card to a computer. Eject the drive on the computer before leaving transfer
mode. CrossPoint stops local filesystem access during export and restarts when
leaving it to restore normal USB serial operation. Other SDK consumers opt in to
`UsbMassStorage` explicitly; this is USB mass-storage device mode.

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

## Touch capture during UI stalls

After `begin()`, Metalio consumers can call `beginBufferedTouch()` and keep
calling `update()` and the existing gesture queries on their main thread.
The SDK task polls the controller every 10 ms through the shared I2C lock;
only the main thread decodes gestures. Capture timestamps, rather than replay
time, determine tap/hold/swipe duration. Downstream gesture timers (such as
HOME double-tap classification) must use `touchFrameTime()` rather than
`millis()`. Idle reports do not count as pending touch activity. This mode is
mutually exclusive with
`beginAsync()`, whose task owns the whole input state and exposes gesture queues.

The bounded queue holds 256 reports in PSRAM (5,120 bytes), covering at least
2.56 seconds at the nominal sample rate. The queue control structure, task
bookkeeping and 4,096-byte task stack stay in internal RAM. Only enabled
Metalio consumers allocate them. Failure returns `false` and leaves synchronous
polling available; unavailable PSRAM does not consume internal RAM as a fallback.
Overflow cancels
pending input and waits for release rather than delivering a partial gesture.

Call `discardBufferedTouch()` when changing screen ownership: queued input and
in-flight old reports are discarded, and a held contact must release before
it can act on the new screen. Regular `suppressTouchContact()` still suppresses
only the current gesture. Stop with `endBufferedTouch()` before changing the
active board or reinitializing its bus. `prepareForDeepSleep()` and destruction
also stop the sampler, allowing its last I2C transfer to finish before cleanup.

Validate short taps during a full refresh, long presses, swipes, two-contact
gestures where supported, HOME/page keys and screen transitions on hardware.
Check all four orientations, free internal heap/largest block, and task stack
headroom during repeated refreshes. Hardware bus/scheduler delays can still
miss a contact that begins and ends between successful reads.
