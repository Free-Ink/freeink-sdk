# Metalio E-Ink4-Plus support

Build flag: `-DFREEINK_DEVICE_METALIO_EINK4_PLUS=1`. Profile: `METALIO_EINK4_PLUS`
in `BoardConfig.h`. Pin roles and power sequences come from the vendor firmware
([CloudZao/Metalio-E-INK4-Plus](https://github.com/CloudZao/Metalio-E-INK4-Plus),
board directory `main/boards/metalio-e-ink4-plus`, hardware rev 1.3/1.4). None of
the SDK support has been validated on hardware yet.

## Toolchain

The board is an ESP32-S31 (RISC-V), which needs Arduino-ESP32 4 on ESP-IDF 6.1.
Build with the pioarduino `61.04.00-RC1` platform (board `esp32-s31-coreboard-1`),
which in turn needs PlatformIO Core 6.2 or newer. See the `metalio_eink4_plus` env
in `platformio.sample.ini`. The S31 is its own MCU family (`FREEINK_MCU_S31`) and
does not share a binary with other boards.

## Hardware

| Block | Wiring |
|---|---|
| Panel | E0470A14-AF, 4.7" 1216x684 raw parallel, 8-bit bus D0-D7 = GPIO8-15, XCL 16, XLE 17, XOE 18, XSTL 4, MODE 19, SPV 35, BORDER 37, CKV 40 |
| Panel power | TPS65185 at 0x68; WAKEUP/PWRUP/VCOM_CTRL on TCA9555 P0.2/P0.1/P0.0, PWR_GOOD on P0.7; VCOM 1200 mV |
| I2C | SDA 0, SCL 1: TCA9555 0x20 (INT GPIO2), FT6336U 0x38, PCF8563 0x51, BQ27220 0x55, CX25601N 0x6B, SC7A20H 0x19 |
| Touch | FT6336U, INT GPIO5, reset TCA9555 P1.2 |
| Keys | BOOT GPIO61 (confirm); volume up/down P1.0/P1.1 and power P1.7 on the TCA9555, active low; cover keys on the touch panel |
| SD | 4-bit SDMMC: CLK 24, CMD 25, D0-D3 20-23 |
| Frontlight | cool GPIO6, warm GPIO7 (YX6016N EN/PWM, 20 kHz, 10-bit) |
| Power off | pulse P1.3 (100 ms high, 100 ms low) until the power switch cuts power |

## Display

The panel is driven by `EpdiyDriver` over the EPDiy submodule at
`libs/display/epdiy` (LGPL-3.0; [Free-Ink/epdiy](https://github.com/Free-Ink/epdiy),
a fork of [vroland/epdiy](https://github.com/vroland/epdiy) 2.1.3 with ESP32-S31
support taken from the vendor port). Run `git submodule update --init
libs/display/epdiy` before building. The board library `BoardMetalioEInk4Plus`
supplies the EPDiy board definition (TPS65185 sequencing through the expander,
LCD bus pins and line timing) and the panel waveforms.

The host renders in the native 1216x684 frame. Full and Half refreshes use GC16,
Fast uses DU. Grayscale uses the overlay masks onto the displayed B/W page (dark
and light gray) with GL16, or GC16 after a Full or Half base. The panel rails are
powered only for each waveform.

## Input

The FT6336U reports the portrait 684x1216 frame. The profile maps it to the
native panel with `swapXY` and `flipY` (the vendor's `x = rawY, y = 683 - rawX`).
The cover-glass keys report as touch points at rawY about 1600: HOME at x 100,
PREV at x 200 (BTN_UP) and NEXT at x 300 (BTN_DOWN), each within 30 counts.

## Not yet supported

Audio (external Bluetooth audio chip on UART 38/39 with I2S 42-45), the
microphone, the CX25601N charger, haptics through the SDK HapticManager (the
motor is expander P0.3; `metalio_plus::vibrate()` drives it directly), and the
USB mass-storage path switch (ANA_SW, GPIO36). The frontlight follows the vendor
in mapping 100% brightness to 10% of the PWM range (`FrontlightConfig::maxPermille`).
