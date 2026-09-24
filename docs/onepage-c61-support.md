# OnePage ESP32-C61 Board Support

## Overview

**OnePage Reader** is an open-hardware DIY E-reader built around the **ESP32-C61** RISC-V wireless SoC.
Main ecosystem repo: [MoveCall/onepage-reader](https://github.com/MoveCall/onepage-reader)

This document describes the FreeInk SDK board support profile for OnePage (`FREEINK_DEVICE_ONEPAGE`).

## Hardware Specifications

| Subsystem | Specification / Pinout |
|:---|:---|
| **SoC** | ESP32-C61 (ESP32-C61HR2, 160MHz RISC-V, Wi-Fi 6, BLE 5.4, 16MB Flash, 2MB PSRAM @ 40MHz) |
| **Display** | 4.26" E-Ink Display (GDEQ0426T82), **SSD1677** controller, **800 × 480** resolution, 4-level grayscale |
| **Display SPI** | SCLK: `GPIO22`, MOSI: `GPIO23`, CS: `GPIO25`, DC: `GPIO8`, RST: `GPIO27`, BUSY: `GPIO29` · **20 MHz** |
| **MicroSD Card** | SPI mode (shared SPI bus with EPD): SCLK: `GPIO22`, MISO: `GPIO24`, MOSI: `GPIO23`, CS: `GPIO26` · **20 MHz** |
| **Buttons (Front 4-Key)** | Single resistor ADC ladder on `GPIO4` (ADC1_CH2):<br>• `BACK`: 2400..2800 mV (~2592 mV)<br>• `LEFT`: 1780..2140 mV (~1956 mV)<br>• `RIGHT`: 1140..1500 mV (~1316 mV)<br>• `CONFIRM` (ENTER): 0..250 mV (~0 mV) |
| **Buttons (Side 3-Key)** | Active-low GPIO buttons with internal pull-up:<br>• `UP` (PREV): `GPIO6`<br>• `DOWN` (NEXT): `GPIO9`<br>• `POWER` (WAKE): `GPIO2` (Deep-sleep wakeup source) |
| **Battery & Charging** | • Battery ADC: `GPIO5` (ADC1_CH3, multiplier ×2.0)<br>• USB / Charge Detect: `GPIO11` (UART0_RXD, LM66200 ST open-drain, low = USB present)<br>• Charge Pause: `GPIO10` (`BAT_CHG_EN`, drive LOW during ADC sample to avoid charger offset) |
| **Power Architecture** | `GPIO27` serves as both EPD RST and shared SD power rail enable. Silenced before deep sleep. |
| **Bluetooth Remote** | BLE HID Central / Host support for wireless page turners |

## FreeInk SDK Profile

In FreeInk SDK (`BoardConfig.h`):

```cpp
constexpr BoardProfile ONEPAGE = {
    Board::OnePage,
    "onepage",
    InputStyle::OnePageAdcLadder,
    DisplayController::SSD1677,
    800,
    480,
    {22, 23, 25, 8, 27, 29, PIN_UNASSIGNED},  // display SPI
    20000000,                                 // 20 MHz
    {22, 24, 23, 26, PIN_UNASSIGNED, false, 20000000, true}, // microSD
    {PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 6, 9, 2, false, 4}, // input
    5,                                         // batteryAdc (GPIO5)
    11,                                        // batteryChargeStatus (GPIO11)
    2.0f,                                      // batteryDividerMultiplier
    11,                                        // usbDetect (GPIO11)
    NO_TOUCH,
    NO_FRONTLIGHT,
    NO_AUDIO,
    NO_LEDS,
    NO_FLIP,
    NO_SDMMC,
    NO_GAUGE,
    NO_MIC,
    NO_SENSORS,
    1.0f,                                      // uiScale
    {PIN_UNASSIGNED, PIN_UNASSIGNED, 10, true},// chargeEnable (GPIO10)
    0,
    {9, 3, 3, 3},
    false                                      // batteryChargeStatusActiveHigh: false
};
```

## Build Configuration

Add the following to your project's `platformio.ini`:

```ini
[env:onepage]
extends = base
board = onepage-c61
board_build.mcu = esp32c61
custom_sdkconfig =
  CONFIG_SPIRAM=y
  CONFIG_SPIRAM_MODE_QUAD=y
  CONFIG_SPIRAM_SPEED_40M=y
  CONFIG_SPIRAM_USE_MALLOC=y
  CONFIG_SPIRAM_BOOT_INIT=y
  CONFIG_SPIRAM_BOOT_HW_INIT=y
  CONFIG_BT_LE_SLEEP_ENABLE=y
  CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
build_flags =
  ${base.build_flags}
  -DFREEINK_DEVICE_ONEPAGE=1
```
