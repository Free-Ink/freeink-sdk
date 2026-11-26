# MindReset Read Pico (RDP-G01-W)

Use `FREEINK_DEVICE_READPICO=1` with the ESP32-S3 N16R8 configuration. The
panel scans 1216 × 684 pixels, with a 684 × 1216 portrait reading surface. The
E0470A01 requires the LCD_CAM, GDMA and RMT backend in `EpdiyLcd`. The board
provides the pin and timing configuration through
`FREEINK_EPDIY_LCD_CONFIG=readPicoEpdiyConfig`.

Call `BoardReadPico::begin()` before starting InputManager, BatteryMonitor,
the RTC or the display. The board enables the shared I²C lock and configures the
FCA9555 expander before the CST836U reset or panel rail sequence. The SY7636A
uses factory VCOM read from the CW32L010. If the PMU cannot supply a valid
value, the driver skips the update. The driver enables VCOM during refresh
and disables it between scans.

The CST836U provides two touch contacts and three separate key zones. The
SC7A20H is an accelerometer. The microSD slot uses 1-bit SDMMC on CLK38, CMD42
and DAT0 44. Select USB Serial/JTAG for the console because UART0 shares
GPIO43 with the touch IRQ and GPIO44 with DAT0.

Release storage and sleep the display and touch before handing off to
`BoardReadPico::pmuSoftSleep()`. The CW32L010 controls the host power rail and
owns wake. The ESP32 has no power-key GPIO to use as a deep-sleep wake source.

The display backend uses PSRAM for the 4-bit current and previous frames,
the 8-bit difference frame, and the B/W display, base and grayscale selector
buffers. These total about 1.98 MiB. Line queues, DMA buffers and scan tasks
use about 54 KiB of internal RAM before allocator and task overhead. The native grayscale image
path borrows the 4-bit current frame.

The board implementation comes from [CrossMux's FreeInk SDK port](https://github.com/0x1abin/freeink-sdk/pull/35)
and its [native grayscale follow-up](https://github.com/0x1abin/freeink-sdk/pull/36).
Hardware references come from [MindReset's reference firmware](https://github.com/MindReset/read_pico_firmware/tree/6a247cc94420b85c650b32a6b2c063a360070eee).
See [NOTICE](../NOTICE) and the [epdiy license](../libs/display/EpdiyLcd/src/epdiy/LICENSE)
for the terms covering the bundled renderer.
