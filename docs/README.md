# FreeInk SDK documentation

Start with the [SDK overview and PlatformIO setup](../README.md#using-freeink-from-platformio).
Device build flags and dependencies are in [platformio.sample.ini](../platformio.sample.ini).

## Integration guides

| Guide | Covers |
|---|---|
| [FreeInkUI](freeink-ui.md) | UI runtime, components, themes, input adapters, and adoption |
| [FreeInkBook](freeink-book.md) | EPUB parsing, layout, fonts, rendering, and caches |
| [Grayscale capabilities](grayscale-capabilities.md) | Overlay and absolute encodings, uploads, and recovery |
| [Deferred refresh migration](deferred-refresh-migration.md) | Split refresh interface and consumer migration |
| [MCU portability](consumer-mcu-portability.md) | Runtime profiles, GPIO wakeup, and C3/S3 differences |
| [BLE keyboard host](ble-keyboard-host.md) | Enabling and using BLE HID input |
| [Testing](testing.md) | Local host regression suites and validation limits |

## Board support

These notes distinguish implemented support, firmware-derived findings, and
hardware validation. See the [supported-device table](../README.md#supported-devices)
for the full device list.

- [Xteink X3 UC8279 variant](xteink-x3-uc8279-support.md)
- [Xteink X4 Pro](xteink-x4pro-support.md)
- [Xteink X4 Classic](xteink-x4c-support.md)
- [EEGO A4](eego-a4-support.md)
- [LilyGo T5 S3](lilygo-t5s3-support.md)
- [M5Stack PaperS3](m5stack-papers3-support.md)
- [OnePage ESP32-C61](onepage-c61-support.md)
- [Waveshare ESP32-S3-ePaper-3.97](waveshare-epaper-397-support.md)

## Driver provenance

- [Display driver reference coverage](display-driver-references.md): source hierarchy and change policy.
- [X4 Pro Licorice firmware audit](xteink-x4pro-licorice-analysis.md): findings tied to specific OEM firmware images.
- [Attribution](../NOTICE) and [license](../LICENSE).
