# Testing FreeInk SDK

Run these commands from the repository root. Host tests use production code
with desktop adapters or hardware stubs; they do not require a device or PlatformIO.
You need a C/C++ compiler with C++17 support, Python 3, and a POSIX shell.

## Serial transport

```sh
python3 libs/hardware/BoardConfig/test/host/run_serial.py
```

Checks that native USB Serial/JTAG output is serviced without a blocking flush,
and that UART, TinyUSB, Sticky's UART bridge, and simulator builds remain no-ops.
Device tests must also verify timely acknowledgements and intact transfer data.

## Paper Mono bring-up

```sh
python3 libs/hardware/BoardConfig/test/host/run_papermono_boot.py
```

Runs `PaperMonoBoard.h` against a model PMIC and IO expander. An expander that
misses its first probes after a PMIC restart must still come up within the boot
wait, whichever manager starts first. A failed setup must stay cached, so no rail
in use is switched off later, and the power button must keep working. A failed
expander read or mode write must not leave the touch controller in reset.

## UI, books, credentials, and input

```sh
sh libs/ui/FreeInkUI/test/host/run.sh
sh libs/book/FreeInkBook/test/host/run.sh
sh libs/book/ContentProtection/test/host/run.sh
sh libs/hardware/InputManager/test/host/run.sh
sh libs/hardware/InputManager/test/host/run_metalio.sh
sh libs/hardware/HapticManager/test/host/run.sh
```

The book suite also needs `zip`. It uses `sips` or ImageMagick's `convert` for
JPEG fixtures and optionally Pillow for progressive JPEG fixtures; the related
checks skip when those converters are unavailable. It builds layout tests for
the default, SMALL, and LARGE memory profiles. Build outputs go under the system
temporary directory.

## Progressive JPEG

```sh
python3 libs/assets/ProgressiveJpeg/test/host/run.py
```

Needs Pillow, `cjpeg` and `djpeg`. Decodes generated progressive JPEGs (4:2:0,
4:2:2, 4:4:4, grayscale, restart intervals, odd sizes) and compares the luma
with `djpeg -grayscale`; also checks that baseline files are refused and that
a truncated stream decodes without failing. Set `PJ_EXTRA_JPEGS` to a
colon-separated list of files to check real-world images too.

## Battery gauge

```sh
sh libs/hardware/BatteryMonitor/test/host/run.sh
```

Compiles `BatteryMonitor` and the real X3 board profile against a model BQ27220
that holds the Design Capacity load to TRM SLUUBD4A 6.1: key timing, CONFIG
UPDATE, checksummed Data Memory writes, and the X3's block re-select. Every
refused I2C transaction must still leave the gauge out of CONFIG UPDATE and
sealed, and the next start must finish the load. It also compiles the EEGO A4
profile against a model CW2015: the battery-profile check and upload sequence,
the voltage and percentage conversion, and the ADC fallback when no gauge
answers.

The battery suite also covers Picco's SGM41562 status as a single register
snapshot, three-conversion ADC median, charging endpoint and completion state,
and the allocation-free `BatteryEstimate` presentation filter. The estimator
checks include unplug settling, low-voltage correction, failed samples, sleep
reconciliation and retained-state validation.
The flash-at-near-full regression covers a 4.340 V cell still reporting active
charging: its stock voltage estimate can reach 100% without inventing charger
completion. Lower voltage held on USB alone does not become full over time.
These are model checks; actual voltage calibration, charge termination and power
usage still need a Picco.

## Motion sensors

```sh
sh libs/hardware/Imu/test/host/run.sh
```

Compiles `Imu` with the Metalio EInk4 profile against a model SC7A20H (and
the QMI8658 and LSM6DS3 scaling), once more with `FREEINK_CAP_IMU=0`, and with
the Paper Mono profile against a model BMI270: `begin()` must only identify
the part, the first `wake()` or `read()` must stream the whole configuration
file through the INIT window in order with a longer bus timeout, a warm part
must skip the upload, and the board's axis flips apply. The model does not run
the configuration file, so the sensor's data path still needs a device.

## Fonts

```sh
sh libs/font/FreeInkFont/test/host/run.sh
sh libs/font/FreeInkFont/test/host/run_ligature.sh
```

Covers `FtFont::RenderOptions` (every `HintingMode` plus monochrome) against a
real bundled font, plus a minimal build with none of the three
`FREEINK_FONT_ENABLE_*` flags set to confirm existing consumers see no change
in output and unsupported requests are reported rather than silently degraded.

Covers `FtFont::ligature()`/`ligatureGlyphId()` (GSUB-based) against a real
bundled font: exact-value assertions for the five standard Latin ligatures,
the codepoint-vs-glyph-ID contract boundary, deinit()/re-init cache
invalidation, and a null-pointer safety check on `Gsub::LigatureGlyphId`.

## BLE HID host

```sh
sh libs/network/BleKeyboardHost/test/host/run.sh
```

Compiles `BleKeyboardHost` with `FREEINK_CAP_BLE_HID_HOST=1` against a fake
NimBLE and FreeRTOS layer, with the connection task on a real thread. Covers
report decode into key events, auto-reconnect, scanning and teardown order.
The fake has no radio, so pairing, link timing and memory use still need a
device.

## Frontlight driver

```sh
sh libs/hardware/FrontlightManager/test/host/run.sh
```

Compiles the Picco frontlight backend and real board profile against a register
model with a wake-settling interval. Covers the first nonzero brightness write,
off/on level restoration, warm/cool channels, slider updates without repeated
wake delays, and retry after a failed wake transaction. Sweeps brightness in both
directions at all 101 warmth settings, checking fine PWM steps, minimum and maximum
output, disabled channels at zero PWM, and restoration after off/on. Physical light
output and the actual controller settling time still require a device check.

## Display drivers

```sh
python3 libs/display/FreeInkDisplay/test/host/run_bus.py
python3 libs/display/FreeInkDisplay/test/host/run_pro.py
python3 libs/display/FreeInkDisplay/test/host/run_uc8279.py
python3 libs/display/FreeInkDisplay/test/host/run_uc8279c_a4.py
python3 libs/display/FreeInkDisplay/test/host/run_uc8253_power.py
python3 libs/display/FreeInkDisplay/test/host/run_picco.py
```

These compile production drivers against recording buses. The Pro suite also
compiles the facade in single- and dual-buffer modes, checks every concrete driver
header against the shared interface, and verifies selected controller identity
independently of board metadata. The Picco suite builds the
SSD1677/SSD2677 drivers as a Picco and as a Sticky, including each board's
bit-banged panel probe. The checks cover transfer
sequences, grayscale uploads, refresh lifecycle, and power-state behavior for
the drivers included in each harness.

For the board-profile and display-probe regression test, follow the
[XteinkDetect host instructions](../libs/hardware/XteinkDetect/test/host/README.md).

## Validation limits

Host suites cover only the code and configurations each harness compiles. Run
consumer firmware builds for affected device and capability combinations, then
validate relevant refresh quality, peripheral behavior, and sleep/wake on hardware.
This SDK supplies sample PlatformIO configurations; firmware entry points belong
to the consumer project.

When checking dead code, distinguish private implementation details from public
SDK APIs. A public method, board profile, compatibility wrapper, or capability-gated
driver can have no in-repository callers and still be used by downstream firmware.
Check references across all configurations before removing internal symbols, and
exclude vendored dependencies and generated assets from routine source cleanup.

## SecureNet bulk transfers

```sh
sh libs/network/SecureNet/test/host/run.sh
WOLFSSL_INTERNAL_SOURCE=/path/to/Arduino-wolfSSL/src/src/internal.c \
  python3 libs/network/SecureNet/test/host/test_record_reservation.py
```

The shell suite exercises HTTP control flow with scripted transports, including
reservation timing after the request, cancellation, and resumed transfers. The
Python harness compiles the production reservation/stop methods and the
consumer's pinned wolfSSL buffer functions with a bounded allocator, sanitizers,
and competing clients. It requires the consumer's verified input-retention and
empty-buffer-release patches. It covers both allocation failure before the fix
and retained medium/full records afterward; it does not perform TLS handshakes
or measure device heap. Consumers without `FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER`
use ordinary on-demand record growth without proactive reservation. Reservations
use wolfSSL's own connection-owned buffer and allocator, with no global allocator
replacement or cross-client loan.

## Streaming JSON previews

```sh
sh libs/network/JsonSax/test/host/run.sh
```

Checks optional bounded string prefixes, unchanged default overflow handling,
continued parsing after long strings, and rejection of oversized keys under ASan/UBSan.
