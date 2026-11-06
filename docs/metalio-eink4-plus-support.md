# Metalio E-Ink4-Plus

Select `-DFREEINK_DEVICE_METALIO_EINK4_PLUS=1`, or copy the `metalio_eink4_plus`
environment from [platformio.sample.ini](../platformio.sample.ini).
The board uses an ESP32-S31 with 16 MB flash and 16 MB octal PSRAM, and needs
pioarduino's ESP-IDF 6.1 platform (61.04.00-RC1 or later). Pins follow the
vendor's `config.h` for board rev 1.3/1.4 in
[CloudZao/Metalio-E-INK4-Plus](https://github.com/CloudZao/Metalio-E-INK4-Plus).
The peripherals below were checked on a physical unit with CrossPoint, except
where [Verification](#verification) says otherwise.

## Supported peripherals

| Peripheral | SDK support |
|---|---|
| E0470A14-AF, 1216×684, no on-glass controller | `EpdiyDriver`: full, half and fast B/W refresh, grayscale overlay (anti-aliasing and images) |
| TPS65185 panel PMIC, 0x68 | Rails and VCOM through the EPDiy board glue (`MetalioPlusBoard`) |
| FT6336U touch, 0x38 | Tap, swipe, hold, two-contact gestures (pinch, rotate, two-finger swipe) and the HOME/PREV/NEXT bezel keys |
| TCA9555, 0x20 | Rails, touch reset, volume and power keys, vibration motor, power-off pulse |
| microSD | Native 4-bit SDMMC through SDCardManager |
| BQ27220, 0x55 | Battery percentage, voltage and charge direction via BatteryMonitor |
| CX25601N charger, 0x6B | Charging state and USB-input presence (read only) |
| PCF8563, 0x51 | Rtc |
| SC7A20H, 0x19 | [Imu](metalio-accelerometer.md): XYZ acceleration; accelerometer only, no gyroscope |
| Warm/cool frontlight | FrontlightManager: two 10-bit LEDC channels, vendor output cap and mix |
| Motor on TCA9555 P0.3 | [HapticManager](haptics.md): on/off pulses and patterns |

Touch, RTC, haptics, IMU, frontlight, the battery gauge and SDMMC enable
automatically. Set `USE_BLOCK_DEVICE_INTERFACE=1` for SdFat's SDMMC
block-device interface and `BOARD_HAS_PSRAM`; the panel buffers live in PSRAM.

USB mass storage (`UsbMassStorage`) is opt-in with `FREEINK_CAP_USB_MSC=1`. The
S31's OTG controller has its own high-speed UTMI PHY, separate from USB
Serial/JTAG, so no PHY handoff is needed: while the drive is up, GPIO36 (the
ANA_SW analog switch) is driven low to route the USB-C port to the OTG PHY, and
high again afterwards, as the vendor does.

Not supported yet: the BH1098G Bluetooth audio module (speaker and microphone),
the 4G modem and the camera connector. The charger is left as the hardware
configures it.

## Pins

| Function | Wiring |
|---|---|
| EPD data | D0–D7 = GPIO8–15 (8-bit bus on the LCD peripheral) |
| EPD control | XCL16, XLE17, XOE18, XSTL4, MODE19, SPV35, CKV40, BORDER37 |
| TPS65185 control | Expander P0.0 VCOM_CTRL, P0.1 PWRUP, P0.2 WAKEUP, P0.7 PWR_GOOD (input) |
| Shared I²C | SDA0, SCL1, 400 kHz: expander, touch, TPS65185, gauge, charger, RTC, IMU |
| Touch | 0x38, INT GPIO5, reset on expander P1.2 |
| SDMMC | CLK24, CMD25, D0–D3 = GPIO20–23; 4-bit |
| Keys | BOOT GPIO61 → confirm; expander P1.0 volume up, P1.1 volume down, P1.7 power (all active low) |
| Frontlight | Cool GPIO6, warm GPIO7 |
| Expander outputs | P0.3 motor, P0.4 amplifier, P0.5 screen power, P1.3 power-switch pulse, P1.4 radio rail |

Expander pin names such as P1.2 mean bit 10, not ESP GPIO numbers. The panel
power line on P0.5 is shared with the camera, and touch reset on P1.2 with the
4G modem.

## Startup and power

`BoardConfig::holdPowerRails()`, the EPDiy board glue, InputManager,
HapticManager and Imu call the idempotent `metalioplus::ensureBooted()`. It
enables the I²C bus lock, powers the screen rail (P0.5) and the radio rail
(P1.4), leaves the TPS65185 asleep and gives the FT6336U a clean reset, then
waits the 300 ms the touch controller needs. The radio rail must stay on:
besides the 4G modem, Bluetooth audio module and amplifier, which the SDK does
not drive, it supplies the TPS65185, which stops answering at 0x68 when the
rail is low. The idle modem on it costs about 11 mA.

To sleep, finish display and storage work, call `display.deepSleep()`, then
`metalioplus::powerOff()`. As in the vendor firmware it toggles the
power-switch line until the switch chip cuts power, with USB connected too; a
power-key press then cold-boots the board, so after a successful cut every wake
is a power-on reset. Plugging USB in does not turn it back on.

If `powerOff()` returns, the switch did not respond. The fallback is a deep
sleep woken by the expander's INT line (GPIO2, shared with the SC7A20H INT1):
`metalioplus::prepareKeyWake()` waits for the power key to be released, reads
the input port to release INT and reports whether INT idles high; then arm
`PowerManager::armWakeOnPins(1ULL << EXPANDER_INT_GPIO, true)` (which keeps the
pull-up powered) and call `PowerManager::deepSleep()`. If INT is held low, stay
awake with `metalioplus::waitForPowerKeyThenRestart()`. Any expander input can
pull INT, so early after a deep-sleep wake, check `metalioplus::powerKeyHeld()`
and sleep again if it is not the power key (CrossPoint does this at the top of
`setup()`).

Light sleep is not usable yet: on the ESP-IDF 6.1 snapshot in pioarduino
61.04.00-RC1 the panel's PSRAM frame buffers come back corrupted. The vendor
firmware backports S31 fixes for this (PSRAM clock handoff to XTAL before
sleep, MPLL recalibration on wake). Measured with the gauge: awake idle about
59 mA at 80 MHz with the light off; light sleep about 19 mA, measured on a
test build in which that corruption occurred, so not an available mode.

## Display

The panel has no controller. EPDiy (the `libs/display/epdiy` submodule,
LGPL-3.0; see its `FREEINK.md`) clocks a 4-bit-per-pixel frame out
of the S31 LCD peripheral, and `EpdiyDriver` expands FreeInk's 1-bit frames
into it. The driver uses the bus itself (`usesExternalBus()`), so `EpdBus` is
never brought up.

Fetch the submodule before building: `git submodule update --init
libs/display/epdiy`.

| Mode | Waveform |
|---|---|
| Full | Black/white clear cycles, then GC16 on every pixel from white (the vendor's deep clean) |
| Half | GC16 on every pixel, unchanged ones included, which cleans the residue DU leaves |
| Fast | DU on changed pixels; a change over a quarter of the screen runs GC16 on the changed pixels, because DU leaves a ghost |
| Grayscale overlay | Two extra grey levels (EPDiy levels 3 and 5) written into the front buffer, drawn with GC16 on the changed pixels |

At `begin()` the driver clears the glass with three black/white cycles, as the
vendor firmware does, and the first frame after that drives every pixel with
GC16 (as Half) whatever the consumer requests: a differential first frame never
drives pixels that stay white, which left a ghost of the sleep image. A Fast
frame that changes nothing is skipped without powering the panel. The TPS65185
rails are dropped after every frame. Consumers should request Half when leaving
grey content (a book page): a changed-pixels update left a ghost of the book in
Home's grey areas.

Grayscale reports `OverlayMasks` with strip uploads. The base must be displayed
first; the planes are then applied to the front buffer the base left behind,
since the facade's framebuffer holds the last plane by then. The panel's GC16
greys are not linear. A flatbed scan of a 16-level ramp put level 3 at about
22% of the way from black to the paper, level 5 at 40%, level 7 at 62% and level
10 at 86%. With levels 5 and 10 the light grey nearly matched the paper and text
looked aliased. Levels 3 and 5 give slightly bold anti-aliasing; 4 and 7 would
split the range evenly, but the difference needs a magnifier to see. VCOM at
1200 and 1700 mV gave the same ramp within a few percent.

Three things the S31 needs:

- **CPU at 320 MHz while drawing.** Arduino boots the S31 at 240 MHz, where the
  two EPDiy line producers cannot keep the LCD stream fed. Consumers raise the
  clock before drawing (CrossPoint does it in its power manager).
- **The pixel-pair lookup.** The driver passes `EPD_LUT_64K`, which selects an
  S31 lookup in `output_common/lut.c`. EPDiy's 1K lookup needs about 37 µs per
  row here, more than the 34 µs each producer has.
- **Internal RAM.** The line queues are 104 rows per producer instead of the
  vendor's 128 (the 192-row prefill needs 96 each); the ~14.6 KB saved pays for
  the ESP-IDF sleep code (~11 KB of IRAM) the power-off fallback links in.

The bootloader must be built by the same ESP-IDF as the application. With a
mismatched bootloader core 0 runs about 170 times slower and the panel stream
fails.

## Touch and keys

The FT6336U sends the same 0x02 point frame as the CST816S, so it runs that
driver's poll path in polling mode (register 0xA4 = 0, as the vendor). The
digitizer is portrait (684×1216 raw) and maps onto the landscape panel as
`x = rawY`, `y = 683 - rawX`. The second contact (registers 0x09–0x0C, ID in
YH[7:4]) feeds the shared multi-touch tracker, so `supportsMultiTouch()` is
true and its pinch, rotate and two-finger swipe classifiers run on this board
(pinch was checked on the device).

Periodic reads recover from missed interrupts and failed first reads.
`touchContactPending()` also reports the initial IRQ and held contacts so idle
loops can keep sampling promptly. The first valid sample accepts a screen
contact, including taps reported in only one sample. The vendor firmware
uses a separate touch task; synchronous consumers can still miss taps during
long main-loop stalls, even when rendering runs on another task.

The bezel keys below the glass report at raw y ≈ 1600:

- HOME (x ≈ 100) emits `wasHomeKeyPressed()`, `wasHomeKeyTapped()` and
  `wasHomeKeyLongPressed()`.
- PREV (x ≈ 200) emits `BTN_UP`; NEXT (x ≈ 300) emits `BTN_DOWN`.
- The volume keys also emit `BTN_UP`/`BTN_DOWN`; the power key is `BTN_POWER`.

## Battery and charger

The BQ27220 gauge reports percentage, voltage and charge direction. The
CX25601N charger is only read. CHG_STAT in register 0x1E bits [4:3] gives the
charging state (01 constant current, 10 constant voltage, 11 top-off). Its
VBUS_STAT field (0x1E bits [2:0]) reads 0 with USB connected, because the vendor
setup turns off D+/D- detection, so USB presence comes from register 0x1F
bit 7, which is set exactly while the cable is out. No charger datasheet is
public: the CHG_STAT and VBUS_STAT layout comes from the vendor's driver, and
the 0x1F bit was found by logging the registers across an unplug.

## Frontlight

Two LEDC channels drive the warm and cool strings at the vendor's 20 kHz,
10-bit PWM settings. Output remains capped at 10% duty (`maxPermille = 100`),
with the vendor's saturating mix: each channel runs at the brightness duty
scaled by min(1, 2 × its share), so neutral lights both strings fully.
The SDK retains its perceptual brightness curve and minimum duty
(`minHoldPermille = 10`); these differ from the vendor's linear brightness ramp.
The cap leaves about 100 PWM steps, so adjacent low brightness settings can
share a duty. Colour and low-brightness behavior need hardware verification.
`FREEINK_FRONTLIGHT_LS` is not supported here
yet: the S31's LEDC has no RC_FAST source, and light sleep is not usable.

## Wi-Fi

At the radio's default transmit power the access point dropped the board for
missing acknowledgements (802.11 reason 34) seconds after it joined, although
it received the AP at about −40 dBm, and NTP replies went missing. The profile
caps transmit power at 15 dBm (`wifiMaxTxPower = 60`). CrossPoint also turns
modem sleep off from station start and limits the station to 802.11b/g/n;
consumers should do the same until the S31's ESP-IDF Wi-Fi driver is final.

## Verification

Checked on a physical unit with CrossPoint: panel quality and grey levels,
grayscale anti-aliasing and images, ghost-free wake, all touch corners,
two-finger pinch, bezel, volume, confirm and power keys, SD access, battery
percentage, charging state and USB presence across unplug and replug, RTC and
NTP, frontlight steps and colour (and restore after wake), vibration,
power-off and wake on battery and on USB (no boot screen on wake), auto-sleep,
a ghost-free Home after leaving a book, and Wi-Fi file transfer.

Not yet checked: the power-off fallback (`prepareKeyWake()`, the INT wake and
the `powerKeyHeld()` re-sleep), which runs only if the switch fails to cut
power; GPIO2's idle level with the accelerometer present; current while powered
off; accelerometer readings and axis orientation (CrossPoint does not use an
accelerometer without a gyroscope); the rotate and two-finger swipe gestures.
FreeInk's existing host suites pass with these changes; no host tests exist yet
for this board.

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
